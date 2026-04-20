#pragma once
#include "draw_command.h"
#include "document_types.h"
#include "layout_cache.h"
#include "theme.h"
#include "pane.h"
#include "ui_constants.h"
#include "memory_resource.h"
#include "search_state.h"
#include <cassert>

// HitTestTextRange をバッファ再利用付きで呼び出し、取得件数を返す。
inline UINT32 FetchHitTestMetrics(IDWriteTextLayout* layout, UINT32 start, UINT32 length,
    std::pmr::vector<DWRITE_HIT_TEST_METRICS>& buffer)
{
    if (buffer.empty()) {
        buffer.resize(8);
    }
    UINT32 count = static_cast<UINT32>(buffer.size());
    HRESULT hr = layout->HitTestTextRange(start, length, 0, 0,
        buffer.data(), count, &count);
    if (hr == E_NOT_SUFFICIENT_BUFFER) {
        buffer.resize(count);
        layout->HitTestTextRange(start, length, 0, 0,
            buffer.data(), count, &count);
    }
    return count;
}

// インラインコードの背景矩形を描画する。
// bgsにはパディング適用済みのレイアウト原点相対矩形が格納されている。
inline void GenInlineCodeBgs(DrawCommandList& cmds,
    const std::pmr::vector<InlineCodeBg>& bgs,
    float origin_x, float origin_y, D2D1_COLOR_F color)
{
    for (const auto& bg : bgs) {
        const D2D1_RECT_F rect = D2D1::RectF(
            origin_x + bg.left,
            origin_y + bg.top,
            origin_x + bg.right,
            origin_y + bg.bottom
        );
        cmds.emplace_back(FillRoundedRectCmd{ rect, INLINE_CODE_CORNER, INLINE_CODE_CORNER, color });
    }
}

// ドキュメントデータとビューポート状態から DrawCommandList を生成する。
// 「何を描画するか」と「どう描画するか」（エグゼキュータ）を分離する。
class CommandGenerator {
public:
    struct Formats {
        IDWriteTextFormat* list_number = nullptr;
        IDWriteTextFormat* icon_font = nullptr;
        IDWriteTextFormat* copy_btn_icon = nullptr;
        IDWriteTextFormat* placeholder_text = nullptr;
    };

    // ファイル切替時にバッファを縮小する
    void ShrinkBuffers()
    {
        if (!shared_hit_test_buffer_) {
            hit_test_buffer_.shrink_to_fit();
        }
    }

    void SetSharedHitTestBuffer(std::pmr::vector<DWRITE_HIT_TEST_METRICS>* buf) noexcept { shared_hit_test_buffer_ = buf; }

    constexpr void SetTheme(const Theme* theme) noexcept
    {
        theme_ = theme;
        cached_is_dark_ = theme->IsDark();
        float a = cached_is_dark_ ? TABLE_STRIPE_ALPHA_DARK : TABLE_STRIPE_ALPHA_LIGHT;
        cached_stripe_color_ = cached_is_dark_
            ? D2D1::ColorF(1.0f, 1.0f, 1.0f, a)
            : D2D1::ColorF(0.0f, 0.0f, 0.0f, a);
    }
    constexpr void SetFormats(const Formats& fmts) noexcept { formats_ = fmts; }
    void SetSearchMatches(const std::pmr::vector<SearchMatch>* matches, int current_index) noexcept
    {
        search_matches_ = matches;
        current_match_index_ = current_index;
    }

    // Markdownコンテンツペインのすべての描画コマンドを生成する。
    // 内部バッファへの参照を返す。次回呼び出しまで有効。
    const DrawCommandList& GenerateMdPane(
        const std::pmr::vector<Node>& nodes, const LayoutCache& cache,
        const PaneRect& md_pane_rect, float scroll_y,
        const TextSelection& selection,
        int first_visible = -1,
        int hovered_copy_node = -1,
        int hovered_save_node = -1,
        float dpi_scale = 1.0f);

private:
    void GenerateNode(DrawCommandList& cmds, const Node& node, const NodeLayoutEntry& entry, const DiagramEntry& diagram, int node_index);

    void GenHorizontalRule(DrawCommandList& cmds, const NodeLayoutEntry& entry, float x, float w);
    void GenTable(DrawCommandList& cmds, const Node& node, const NodeLayoutEntry& entry, int node_index, float x);
    void GenTableRowBg(DrawCommandList& cmds, bool is_header, bool is_even_row, float x, float y, float table_width, float row_h, float border);
    void GenTableCellContent(DrawCommandList& cmds, const TableCell& cell, IDWriteTextLayout* cell_layout, float text_x, float text_y, bool has_selection, uint32_t sel_start, uint32_t sel_end, uint32_t flat_offset);
    void GenCodeBlockBg(DrawCommandList& cmds, const NodeLayoutEntry& entry, float x, float w);
    void GenOverlayButton(DrawCommandList& cmds, D2D1_RECT_F btn, wchar_t icon, bool is_hovered);
    void GenCopyButton(DrawCommandList& cmds, const NodeLayoutEntry& entry, float x, float w, bool is_hovered);
    void GenSaveButton(DrawCommandList& cmds, float bitmap_right, float bitmap_top, bool is_hovered);
    void GenListBullet(DrawCommandList& cmds, const Node& node, const NodeLayoutEntry& entry, float x);
    void GenBlockQuoteGroupDecorations(DrawCommandList& cmds, const std::pmr::vector<Node>& nodes, const LayoutCache& cache, int node_count, int first_visible);
    void GenDiagramPlaceholder(DrawCommandList& cmds, float x, float y, float w, float h);
    void EmitHighlightRects(DrawCommandList& cmds, IDWriteTextLayout* layout, uint32_t start, uint32_t length, float origin_x, float origin_y, D2D1_COLOR_F color);
    void GenSelectionHighlight(DrawCommandList& cmds, IDWriteTextLayout* layout, uint32_t start, uint32_t length, float origin_x, float origin_y);
    void GenSearchHighlights(DrawCommandList& cmds, IDWriteTextLayout* layout, int node_index, float origin_x, float origin_y, int table_row = -1, int table_col = -1);

    const Theme* theme_ = nullptr;
    Formats formats_;

    // 偶数/奇数フレームで交互に使うダブルバッファ monotonic リソース。
    // 前フレームのコマンドリストが次フレーム開始時点まで有効に保たれる。
    MonotonicResource frame_resource_{ 128 * 1024 };
    MonotonicResource frame_resource_alt_{ 128 * 1024 };
    DrawCommandList cmds_{ frame_resource_.resource() };
    int active_buffer_ = 0;
    MonotonicResource& frame_resource() noexcept
    {
        return active_buffer_ == 0 ? frame_resource_ : frame_resource_alt_;
    }

    const std::pmr::vector<SearchMatch>* search_matches_ = nullptr;
    int current_match_index_ = -1;

    std::pmr::vector<DWRITE_HIT_TEST_METRICS>* shared_hit_test_buffer_ = nullptr;
    std::pmr::vector<DWRITE_HIT_TEST_METRICS> hit_test_buffer_;
    std::pmr::vector<DWRITE_HIT_TEST_METRICS>& GetHitTestBuffer() noexcept
    {
        return shared_hit_test_buffer_ ? *shared_hit_test_buffer_ : hit_test_buffer_;
    }

    DrawTextCmd MakeTextCmd(const wchar_t* src, size_t len, D2D1_RECT_F r, IDWriteTextFormat* fmt, D2D1_COLOR_F col)
    {
        assert(len <= 255 && "DrawTextCmd text exceeds uint8_t range");
        DrawTextCmd c{};
        c.text_len = static_cast<uint8_t>((std::min)(len, size_t(255)));
        if (c.text_len > 0) {
            auto* buf = static_cast<wchar_t*>(frame_resource().resource()->allocate(c.text_len * sizeof(wchar_t), alignof(wchar_t)));
            std::char_traits<wchar_t>::copy(buf, src, c.text_len);
            c.text = buf;
        }
        c.rect = r;
        c.format = fmt;
        c.color = col;
        return c;
    }

    D2D1_COLOR_F cached_stripe_color_{};
    bool cached_is_dark_ = false;

    // GenerateMdPane のスコープ内で不変のフレーム単位コンテキスト。
    // GenerateNode 等の private 関数から参照される。
    float frame_offset_x_ = 0.0f;
    float frame_viewport_top_ = 0.0f;
    float frame_viewport_bottom_ = 0.0f;
    // フレーム座標系でのビューポート左右端。各セル x を frame_offset_x_ と
    // 合成したあとカリング判定に使う。GenerateMdPane で
    // -theme_->margin_left / md_pane_rect.width - theme_->margin_left を設定。
    float frame_viewport_left_ = 0.0f;
    float frame_viewport_right_ = 0.0f;
    float frame_content_width_ = 0.0f;
    const TextSelection* frame_selection_ = nullptr;
    int frame_hovered_copy_node_ = -1;
    int frame_hovered_save_node_ = -1;
};
