#include "pane_layout.h"
#include <algorithm>

PaneLayout ComputePaneLayout(float total_width, float total_height,
    float file_pane_width, float toc_pane_width,
    float splitter_width, bool show_file, bool show_toc,
    float md_min_width, float top_offset) noexcept
{
    PaneLayout layout{};
    float x = 0.0f;
    float pane_height = total_height - top_offset;
    if (pane_height < 0.0f) {
        pane_height = 0.0f;
    }

    // ファイルペイン
    if (show_file) {
        layout.file_rect = { x, top_offset, file_pane_width, pane_height };
        x += file_pane_width + splitter_width;
    }

    // 目次ペイン
    if (show_toc) {
        layout.toc_rect = { x, top_offset, toc_pane_width, pane_height };
        x += toc_pane_width + splitter_width;
    }

    // MDペインは残りの幅を使用
    float md_width = std::max(md_min_width, total_width - x);
    layout.md_rect = { x, top_offset, md_width, pane_height };

    return layout;
}

PaneZone DetectPaneZone(float dip_x, const PaneLayout& layout, float splitter_width, bool show_file, bool show_toc) noexcept
{
    if (show_file) {
        float s1_x = layout.file_rect.x + layout.file_rect.width;
        if (dip_x >= layout.file_rect.x && dip_x < s1_x) {
            return PaneZone::FilePane;
        }
        if (dip_x >= s1_x && dip_x < s1_x + splitter_width) {
            return PaneZone::Splitter1;
        }
    }

    if (show_toc) {
        float s2_x = layout.toc_rect.x + layout.toc_rect.width;
        if (dip_x >= layout.toc_rect.x && dip_x < s2_x) {
            return PaneZone::TocPane;
        }
        if (dip_x >= s2_x && dip_x < s2_x + splitter_width) {
            return PaneZone::Splitter2;
        }
    }

    if (dip_x >= layout.md_rect.x) {
        return PaneZone::MdPane;
    }

    return PaneZone::None;
}

PaneScrollInfo ComputeScrollInfo(const PaneRect& rect, float header_height, float total_content, float thumb_min) noexcept
{
    PaneScrollInfo info{};
    info.content_top = rect.y + header_height;
    info.content_height = rect.height - header_height;
    info.total_content = total_content;
    info.max_scroll = std::max(0.0f, total_content - info.content_height);
    float thumb_ratio = (total_content > 0) ? info.content_height / total_content : 1.0f;
    info.thumb_height = std::min(info.content_height, std::max(thumb_min, info.content_height * thumb_ratio));
    return info;
}

float ComputeThumbY(const PaneScrollInfo& info, float scroll_y) noexcept
{
    float scroll_ratio = (info.max_scroll > 0) ? scroll_y / info.max_scroll : 0.0f;
    return info.content_top + scroll_ratio * (info.content_height - info.thumb_height);
}

float ScrollFromThumbY(const PaneScrollInfo& info, float thumb_y) noexcept
{
    float track_range = info.content_height - info.thumb_height;
    float ratio = (track_range > 0) ? (thumb_y - info.content_top) / track_range : 0.0f;
    ratio = std::clamp(ratio, 0.0f, 1.0f);
    return ratio * info.max_scroll;
}
