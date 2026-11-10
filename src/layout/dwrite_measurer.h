#pragma once
#include "doc_dwrite_bridge.h"
#include "layout.h"
#include <dwrite.h>
#include <wrl/client.h>
#include <cstdint>
#include <span>
#include <thread>

class InlineMathTable;
class InlineImageTable;

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
    // 無ければ画像を alt の文字のまま組む。
    void SetInlineImages(InlineImageTable* table) noexcept
    {
        inline_images_ = table;
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
    // run の書式と、数式・画像のインラインオブジェクトの掛け先。
    struct RunFormat {
        // 画像の src を引くノード (表はセルの run も表のノードで引く)。
        const Node& owner;
        // layout の書式。読めない画像の alt を同じ書式で組む。
        const TextStyle& style;
        // false はインラインコードを縮小しない (見出し用)。CodeBlock は run を持たない。
        bool apply_code_size = true;
        // 置く場所の文字の大きさ。数式のキーと、読めない画像のアイコンの大きさに使う。
        float font_size = 0.0f;
        // > 0 なら、それより広い数式をその幅に縮める (ブロック数式)。
        float math_fit_width = 0.0f;
        // > 0 なら、それより広い画像をその幅に縮める。
        float image_fit_width = 0.0f;
    };
    struct RunFormatResult {
        // 寸法がまだ無いインライン数式か画像があり、仮に組んだ (InvalidatePendingInlineLayouts で組み直す)。
        bool pending = false;
        // NodeLayoutEntry::image_min_width。
        float image_min_width = 0.0f;
        // TableLayoutData::ImageCell::image_width。
        float widest_image = 0.0f;
    };
    RunFormatResult ApplyRunFormatting(IDWriteTextLayout* layout, std::span<const TextRun> runs, std::string_view text, const RunFormat& format) const;
    // MeasureTableCells・FinalizeTableLayout・RestoreEvictedTableRows で共通。空セルは null。仮に組んだ数式か画像があれば pending_inline を立てる。
    // image_fit_width > 0 なら、それより広い画像をその幅に縮める (列幅を決めた後)。image_cell があれば min_width と image_width を書く。
    Microsoft::WRL::ComPtr<IDWriteTextLayout> CreateCellLayout(const DWriteContext& ctx, const Node& node, bool& pending_inline, size_t r, size_t c,
                                                               float image_fit_width, TableLayoutData::ImageCell* image_cell = nullptr) const;
    // 全セルの自然幅と折り返さない高さを測り、列ごとの自然幅と最小幅を tl.natural_col_widths・tl.min_col_widths に集計する。
    // 大きな表はセル layout を残さず、全行を evict 済みにする。
    void MeasureTableCells(const Node& node, TableLayoutData& tl, bool& pending_inline) const;
    // 戻り値はテーブル全体の高さ。画像を含むセルは決めた列幅で組み直す。
    float FinalizeTableLayout(const Node& node, TableLayoutData& tl, float max_width, bool& pending_inline) const;

    DWriteContext ctx_;
    // 本文と見出し (theme.font_family)、コードブロック (theme.monospace_font) の書式の対応。フォールバック先は主フォントでも変わる。
    FallbackFontMap body_fonts_;
    FallbackFontMap code_fonts_;
    // RecreateFormats を呼んだスレッド。並列計測中には変わらない (RecreateFormats と同じ契約)。
    std::thread::id owner_thread_;
    InlineMathTable* inline_math_ = nullptr;
    InlineImageTable* inline_images_ = nullptr;
};
