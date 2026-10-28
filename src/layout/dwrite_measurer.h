#pragma once
#include "layout.h"
#include <dwrite.h>
#include <wrl/client.h>
#include <cstdint>
#include <span>
#include <thread>

struct NodeTableData;
struct TableLayoutData;
class InlineMathTable;
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

    std::optional<float> MeasureNode(
        const Node& node,
        NodeLayoutEntry& entry,
        float max_width,
        YRange viewport) const override;
    TableRestoreResult RestoreEvictedTableRows(
        const Node& node,
        NodeLayoutEntry& entry,
        float max_width,
        YRange viewport) const override;

    // scheduler のワーカーを起こし、各書式で日本語を含む小さなレイアウトを作って捨てる。
    // 最初の計測がフォントの読み込みとフォールバックの解決を待たないよう、起動中の他の処理と重ねる。
    // ワーカーごとのファクトリと書式もここで作る。
    void WarmUp(TaskScheduler& scheduler) const;

    // 外部のIDWriteFactoryで初期化する（RecreateFormats の前に呼び出す必要がある）。
    void SetFactory(IDWriteFactory* factory) noexcept
    {
        ctx_.factory = factory;
    }
    // 無ければインライン数式を原文のまま組む。
    void SetInlineMath(InlineMathTable* table) noexcept
    {
        inline_math_ = table;
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
    // 呼び出しスレッド専用のファクトリと書式。generation が変わったら theme で作り直す。作れなければ nullptr。
    static const DWriteContext* ThreadContext(const Theme& theme, uint64_t generation);
    static IDWriteTextFormat* GetTextFormat(const DWriteContext& ctx, const Node& node) noexcept;
    static IDWriteTextFormat* GetTableRowFormat(const DWriteContext& ctx, const NodeTableData& tbl, size_t r) noexcept;
    std::optional<float> MeasureTable(const Node& node, NodeLayoutEntry& entry, float max_width, YRange viewport) const;
    // apply_code_size=false はインラインコードを縮小しない (見出し用)。CodeBlock は run を持たない。
    // 戻り値: 寸法がまだ無く原文のまま組んだインライン数式がある。
    bool ApplyRunFormatting(IDWriteTextLayout* layout, std::span<const TextRun> runs, std::string_view text, bool apply_code_size) const;
    // 寸法が分かっているインライン数式をインラインオブジェクトに置き換える。他の書式の後に呼ぶ (数式の文字の大きさを読む)。
    bool ApplyInlineMath(IDWriteTextLayout* layout, std::span<const TextRun> runs, std::string_view text) const;
    // MeasureTableCells / BuildRowCells 共通のセル生成処理。空セルは null。
    Microsoft::WRL::ComPtr<IDWriteTextLayout> CreateCellLayout(const DWriteContext& ctx, const NodeTableData& tbl, TableLayoutData& tl, size_t r, size_t c) const;
    // 全セルの自然幅と折り返さない高さを測り、列ごとの自然幅を tl.natural_col_widths に集計する。
    // 大きな表はセル layout を残さず、全行を evict 済みにする。
    void MeasureTableCells(const NodeTableData& tbl, TableLayoutData& tl) const;
    // viewport (ローカル座標) 内の evict 行のセルを作り直して evict を解き、戻した行を昇順で返す。
    // row_heights を渡せば、今の列幅での行高さを戻した行の順に書く。
    std::vector<size_t> RestoreRowsInViewport(const NodeTableData& tbl, TableLayoutData& tl, YRange viewport,
                                                   std::vector<float>* row_heights) const;
    // evict 行 r の null セルを再生成する。書くのは行 r のセルだけなので、別の行とは並列に呼べる。
    void BuildRowCells(const NodeTableData& tbl, TableLayoutData& tl, size_t r) const;
    // 戻り値はテーブル全体の高さ。
    float FinalizeTableLayout(const NodeTableData& tbl, TableLayoutData& tl, float max_width) const;

    DWriteContext ctx_;
    // RecreateFormats を呼んだスレッド。並列計測中には変わらない (RecreateFormats と同じ契約)。
    std::thread::id owner_thread_;
    InlineMathTable* inline_math_ = nullptr;
};
