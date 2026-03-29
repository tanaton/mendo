#pragma once
#include "draw_command.h"
#include "types.h"
#include "layout_cache.h"
#include "theme.h"
#include "pane.h"
#include "memory_resource.h"

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
    void ShrinkBuffers() { hit_test_buffer_.shrink_to_fit(); }

    constexpr void SetTheme(const Theme* theme) noexcept
    {
        theme_ = theme;
        cached_is_dark_ = theme->IsDark();
        float a = cached_is_dark_ ? 0.05f : 0.02f;
        cached_stripe_color_ = cached_is_dark_
            ? D2D1::ColorF(1.0f, 1.0f, 1.0f, a)
            : D2D1::ColorF(0.0f, 0.0f, 0.0f, a);
    }
    constexpr void SetFormats(const Formats& fmts) noexcept { formats_ = fmts; }

    // Markdownコンテンツペインのすべての描画コマンドを生成する。
    // 内部バッファへの参照を返す。次回呼び出しまで有効。
    const DrawCommandList& GenerateMdPane(
        const std::pmr::vector<Node>& nodes, const LayoutCache& cache,
        const PaneRect& md_pane_rect, float scroll_y,
        const TextSelection& selection,
        int first_visible = -1,
        int hovered_copy_node = -1,
        float dpi_scale = 1.0f);

private:
    void GenerateNode(DrawCommandList& cmds,
        const Node& node, const NodeLayoutEntry& entry, const DiagramEntry& diagram,
        int node_index, float offset_x, float viewport_top, float viewport_bottom,
        const TextSelection& selection, float content_width,
        int hovered_copy_node);

    void GenHorizontalRule(DrawCommandList& cmds, const NodeLayoutEntry& entry, float x, float w);
    void GenTable(DrawCommandList& cmds, const Node& node, const NodeLayoutEntry& entry,
        int node_index, float x, const TextSelection& selection,
        float viewport_top, float viewport_bottom);
    void GenTableRowBg(DrawCommandList& cmds, bool is_header, bool is_even_row,
        float x, float y, float table_width, float row_h, float border);
    void GenTableCellContent(DrawCommandList& cmds, const TableCell& cell,
        IDWriteTextLayout* cell_layout,
        float text_x, float text_y,
        bool has_selection, uint32_t sel_start, uint32_t sel_end,
        uint32_t flat_offset);
    void GenCodeBlockBg(DrawCommandList& cmds, const NodeLayoutEntry& entry, float x, float w);
    void GenCopyButton(DrawCommandList& cmds, const NodeLayoutEntry& entry,
        float x, float w, bool is_hovered);
    void GenListBullet(DrawCommandList& cmds, const Node& node, const NodeLayoutEntry& entry, float x);
    void GenBlockQuoteGroupDecorations(DrawCommandList& cmds,
        const std::pmr::vector<Node>& nodes, const LayoutCache& cache,
        int node_count, float offset_x, float content_width,
        int first_visible, float viewport_bottom);
    void GenDiagramPlaceholder(DrawCommandList& cmds, float x, float y, float w, float h);
    void GenSelectionHighlight(DrawCommandList& cmds, IDWriteTextLayout* layout,
        uint32_t start, uint32_t length, float origin_x, float origin_y);

    const Theme* theme_ = nullptr;
    Formats formats_;

    // フレーム毎にリセットする monotonic リソースで描画コマンドを管理
    MonotonicResource frame_resource_{ 128 * 1024 };
    DrawCommandList cmds_{ frame_resource_.resource() };

    std::pmr::vector<DWRITE_HIT_TEST_METRICS> hit_test_buffer_;
    D2D1_COLOR_F cached_stripe_color_{};
    bool cached_is_dark_ = false;
};
