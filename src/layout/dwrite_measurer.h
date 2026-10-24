#pragma once
#include "doc_dwrite_bridge.h"
#include "text_measurer.h"
#include <dwrite.h>
#include <wrl/client.h>
#include <cstdint>
#include <memory_resource>
#include <span>
#include <thread>

struct NodeTableData;
struct TableLayoutData;
class TaskScheduler;

// ITextMeasurer の DirectWrite 実装。IDWriteTextFormat (重い再利用可能オブジェクト) を
// theme ごとに所有し、計測のたびに per-call で IDWriteTextLayout を生成する分業構造。
// Format の再生成は RecreateFormats 経由のみ許可し、並列計測中の同時更新を契約で禁止する。
// 書式を作ったスレッド (UI) 以外は、自分専用の ISOLATED ファクトリと書式で測る。共有ファクトリを全スレッドで使うと
// ファクトリ内部の競合で 4 本を超えると遅くなる (test.md x8 の全計測で、共有は 8 本 705ms・16 本 1480ms、
// 分けると 410ms・260ms)。書式もファクトリに結び付いているので、スレッドごとに作る。
class DWriteTextMeasurer : public ITextMeasurer {
public:
    bool RecreateFormats() override;
    void UpdateTheme(const Theme& theme) noexcept override
    {
        theme_ = &theme;
    }

    void MeasureNode(
        const Node& node,
        NodeLayoutEntry& entry,
        float max_width,
        MeasureViewportRange viewport) const override;
    TableRestoreResult RestoreEvictedTableRows(
        const Node& node,
        NodeLayoutEntry& entry,
        float max_width,
        MeasureViewportRange viewport) const override;

    // scheduler のワーカーを起こし、各書式で日本語を含む小さなレイアウトを作って捨てる。
    // 最初の計測がフォントの読み込みとフォールバックの解決を待たないよう、起動中の他の処理と重ねる。
    // ワーカーごとのファクトリと書式もここで作る。
    void WarmUp(TaskScheduler& scheduler) const;

    // 外部のIDWriteFactoryで初期化する（RecreateFormats の前に呼び出す必要がある）。
    void SetFactory(IDWriteFactory* factory) noexcept
    {
        ctx_.factory = factory;
    }
    // 巨大テーブルのセル生成を並列化する先。nullptr なら直列。LayoutEngine と同じ契約で
    // scheduler の Shutdown より前に nullptr へ戻すこと。
    void SetScheduler(TaskScheduler* scheduler) noexcept
    {
        scheduler_ = scheduler;
    }

private:
    struct DWriteContext {
        Microsoft::WRL::ComPtr<IDWriteFactory> factory;
        Microsoft::WRL::ComPtr<IDWriteTextFormat> body;
        Microsoft::WRL::ComPtr<IDWriteTextFormat> h[6];
        Microsoft::WRL::ComPtr<IDWriteTextFormat> code;
        // RecreateFormats ごとに全インスタンスで一意な番号。ワーカーの書式が古いかの判定に使う。
        uint64_t generation = 0;
    };

    static bool CreateFormats(const Theme& theme, DWriteContext& ctx);
    // 呼び出しスレッドで使うファクトリと書式。書式を作ったスレッドは ctx_ (共有ファクトリ)。
    const DWriteContext& Context() const;
    static IDWriteTextFormat* GetTextFormat(const DWriteContext& ctx, const Node& node) noexcept;
    static IDWriteTextFormat* GetTableRowFormat(const DWriteContext& ctx, const NodeTableData& tbl, size_t r) noexcept;
    void MeasureTable(const Node& node, NodeLayoutEntry& entry, float max_width, MeasureViewportRange viewport) const;
    // apply_code_size=false はインラインコードを縮小しない (見出し用)。CodeBlock は run を持たない。
    void ApplyRunFormatting(IDWriteTextLayout* layout, std::span<const TextRun> runs, std::string_view text, bool apply_code_size) const;
    // MeasureTableCells / BuildRowCells 共通のセル生成処理。空セルは null。
    Microsoft::WRL::ComPtr<IDWriteTextLayout> CreateCellLayout(const DWriteContext& ctx, const NodeTableData& tbl, size_t r, size_t c) const;
    // 全セルの自然幅と折り返さない高さを測り、列ごとの自然幅を tl.natural_col_widths に集計する。
    // 大きな表はセル layout を残さず、全行を evict 済みにする。
    void MeasureTableCells(const NodeTableData& tbl, TableLayoutData& tl) const;
    void RestoreNullCellLayouts(const NodeTableData& tbl, TableLayoutData& tl, MeasureViewportRange viewport) const;
    // evict 行 r の null セルを再生成する。書くのは行 r のセルだけなので、別の行とは並列に呼べる。
    void BuildRowCells(const NodeTableData& tbl, TableLayoutData& tl, size_t r) const;
    void FinalizeTableLayout(const NodeTableData& tbl, NodeLayoutEntry& entry, float max_width) const;

    const Theme* theme_ = nullptr;
    TaskScheduler* scheduler_ = nullptr;
    DWriteContext ctx_;
    // RecreateFormats を呼んだスレッド。並列計測中には変わらない (RecreateFormats と同じ契約)。
    std::thread::id owner_thread_;
};
