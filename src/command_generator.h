#pragma once
#include "draw_command.h"
#include "types.h"
#include "layout_cache.h"
#include "theme.h"
#include "pane.h"
#include "memory_resource.h"

// Generates DrawCommandList from document data and viewport state.
// Separates "what to draw" from "how to draw it" (the executor).
class CommandGenerator {
public:
    struct Formats {
        IDWriteTextFormat* list_number = nullptr;
        IDWriteTextFormat* icon_font = nullptr;
    };

    void SetTheme(const Theme* theme) noexcept {
        theme_ = theme;
        bool is_dark = (theme->bg_color.r + theme->bg_color.g + theme->bg_color.b) < 1.5f;
        float a = is_dark ? 0.05f : 0.02f;
        cached_stripe_color_ = is_dark
            ? D2D1::ColorF(1.0f, 1.0f, 1.0f, a)
            : D2D1::ColorF(0.0f, 0.0f, 0.0f, a);
    }
    void SetFormats(const Formats& fmts) noexcept { formats_ = fmts; }

    // Generate all draw commands for the Markdown content pane.
    // Returns a reference to an internal buffer; valid until the next call.
    const DrawCommandList& GenerateMdPane(
        const std::pmr::vector<Node>& nodes, const LayoutCache& cache,
        const PaneRect& md_pane_rect, float scroll_y,
        const TextSelection& selection,
        int first_visible = -1);

private:
    void GenerateNode(DrawCommandList& cmds,
        const Node& node, const NodeLayoutEntry& entry, const DiagramEntry& diagram,
        int node_index, float offset_x, float viewport_top, float viewport_bottom,
        const TextSelection& selection, float content_width);

    void GenHorizontalRule(DrawCommandList& cmds, const NodeLayoutEntry& entry, float x, float w);
    void GenTable(DrawCommandList& cmds, const Node& node, const NodeLayoutEntry& entry,
                  int node_index, float x, const TextSelection& selection);
    void GenCodeBlockBg(DrawCommandList& cmds, const NodeLayoutEntry& entry, float x, float w);
    void GenListBullet(DrawCommandList& cmds, const Node& node, const NodeLayoutEntry& entry, float x);
    void GenBlockQuoteBar(DrawCommandList& cmds, const NodeLayoutEntry& entry, float base_x);
    void GenSelectionHighlight(DrawCommandList& cmds, IDWriteTextLayout* layout,
                               uint32_t start, uint32_t length, float origin_x, float origin_y);

    const Theme* theme_ = nullptr;
    Formats formats_;

    // フレーム毎にリセットする monotonic リソースで描画コマンドを管理
    FrameMonotonicResource frame_resource_{128 * 1024};
    DrawCommandList cmds_{frame_resource_.resource()};

    std::pmr::vector<DWRITE_HIT_TEST_METRICS> hit_test_buffer_;
    D2D1_COLOR_F cached_stripe_color_{};
};
