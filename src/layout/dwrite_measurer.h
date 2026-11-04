#pragma once
#include "doc_dwrite_bridge.h"
#include "layout.h"
#include <dwrite.h>
#include <wrl/client.h>
#include <cstdint>
#include <span>
#include <thread>

struct NodeTableData;
struct TableLayoutData;
class InlineMathTable;

// ITextMeasurer の DirectWrite 実装。IDWriteTextFormat (重い再利用可能オブジェクト) を
// theme ごとに所有し、計測のたびに per-call で IDWriteTextLayout を生成する分業構造。
// Format の再生成は RecreateFormats 経由のみ許可し、並列計測中の同時更新を契約で禁止する。
// 書式を作ったスレッド (UI) 以外は、自分専用の ISOLATED ファクトリと書式で測る。共有ファクトリを全スレッドで使うと
// ファクトリ内部の競合で 4 本を超えると遅くなる (test.md x8 の全計測で、共有は 8 本 705ms・16 本 1480ms、
// 分けると 410ms・260ms)。書式もファクトリに結び付いているので、スレッドごとに作る。
class DWriteTextMeasurer : public ITextMeasurer {
public:
    bool RecreateFormats() override;

    std::optional<float> MeasureNode(const Node& node, NodeLayoutEntry& entry, float max_width) const override;
    TableRestoreResult RestoreEvictedTableRows(const Node& node, NodeLayoutEntry& entry, YRange viewport) const override;

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
    // theme.font_family と theme.text_locale の書式の対応。
    const FallbackFontMap& BodyFonts() const noexcept
    {
        return body_fonts_;
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
    // 呼び出しスレッドで使うファクトリと書式。書式を作ったスレッドは ctx_ (共有ファクトリ)、ワーカーは専用のものを最初の呼び出しで作る。
    // 起動時に全ワーカーへ先に作らせない。最初の計測は D3D デバイスの作成の裏で終わるので速くならず、
    // 作成と競合して最初のフレームが 1〜5ms 遅れ、使わないワーカーの分だけ private も 1〜3MB 増える。
    const DWriteContext& Context() const;
    // 書式と、その書式の主フォントに合う FallbackFontMap。合わない組み合わせは字形を黙って変える。
    struct TextStyle {
        IDWriteTextFormat* format;
        const FallbackFontMap& fonts;
    };
    TextStyle NodeStyle(const DWriteContext& ctx, const Node& node) const noexcept;
    TextStyle TableRowStyle(const DWriteContext& ctx, const NodeTableData& tbl, size_t r) const noexcept;
    std::optional<float> MeasureTable(const Node& node, NodeLayoutEntry& entry, float max_width) const;
    // apply_code_size=false はインラインコードを縮小しない (見出し用)。CodeBlock は run を持たない。
    // fit_width > 0 なら、それより広い数式をその幅に縮める (ブロック数式)。
    // 戻り値: 寸法がまだ無く原文のまま組んだインライン数式がある。
    bool ApplyRunFormatting(IDWriteTextLayout* layout, std::span<const TextRun> runs, std::string_view text, bool apply_code_size, float math_font_size, float fit_width) const;
    // MeasureTableCells / RestoreTableRows 共通のセル生成処理。空セルは null。
    Microsoft::WRL::ComPtr<IDWriteTextLayout> CreateCellLayout(const DWriteContext& ctx, const NodeTableData& tbl, TableLayoutData& tl, size_t r, size_t c) const;
    // 全セルの自然幅と折り返さない高さを測り、列ごとの自然幅を tl.natural_col_widths に集計する。
    // 大きな表はセル layout を残さず、全行を evict 済みにする。
    void MeasureTableCells(const NodeTableData& tbl, TableLayoutData& tl) const;
    // 戻り値はテーブル全体の高さ。
    float FinalizeTableLayout(const NodeTableData& tbl, TableLayoutData& tl, float max_width) const;

    DWriteContext ctx_;
    // 本文と見出し (theme.font_family)、コードブロック (theme.monospace_font) の書式の対応。フォールバック先は主フォントでも変わる。
    FallbackFontMap body_fonts_;
    FallbackFontMap code_fonts_;
    // RecreateFormats を呼んだスレッド。並列計測中には変わらない (RecreateFormats と同じ契約)。
    std::thread::id owner_thread_;
    InlineMathTable* inline_math_ = nullptr;
};
