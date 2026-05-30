#include "pane_layout.h"
#include <algorithm>

PaneLayout ComputePaneLayout(
    float total_width, float total_height,
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

    // 保存幅がウィンドウより広い場合 (広いモニタで保存→狭い画面で起動した等) でも
    // MD ペインとスプリッタを画面内に保つため、表示上の side 幅を clamp する。論理幅
    // (PaneController::widths_) は変更しないので、ウィンドウを広げれば元に戻る。
    // side がウィンドウに収まる通常時は no-op。
    float fw = show_file ? file_pane_width : 0.0f;
    float tw = show_toc ? toc_pane_width : 0.0f;
    const float splitters = (show_file ? splitter_width : 0.0f) + (show_toc ? splitter_width : 0.0f);
    const float avail_sides = total_width - md_min_width - splitters;
    if (fw + tw > avail_sides) {
        if (avail_sides <= 0.0f) {
            fw = 0.0f;
            tw = 0.0f;
        }
        else {
            const float scale = avail_sides / (fw + tw); // fw + tw > avail_sides > 0
            fw *= scale;
            tw *= scale;
        }
    }

    if (show_file) {
        layout.file_rect = { x, top_offset, fw, pane_height };
        x += fw + splitter_width;
    }

    if (show_toc) {
        layout.toc_rect = { x, top_offset, tw, pane_height };
        x += tw + splitter_width;
    }

    const float md_width = std::max(md_min_width, total_width - x);
    layout.md_rect = { x, top_offset, md_width, pane_height };

    return layout;
}

PaneZone DetectPaneZone(float dip_x, const PaneLayout& layout, float splitter_width, bool show_file, bool show_toc) noexcept
{
    if (show_file) {
        const float s1_x = layout.file_rect.x + layout.file_rect.width;
        if (dip_x >= layout.file_rect.x && dip_x < s1_x) {
            return PaneZone::FilePane;
        }
        if (dip_x >= s1_x && dip_x < s1_x + splitter_width) {
            return PaneZone::Splitter1;
        }
    }

    if (show_toc) {
        const float s2_x = layout.toc_rect.x + layout.toc_rect.width;
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
    // 負を許すと max_scroll が過大化し thumb_height も負側へ倒れて scrollbar 計算が連鎖破綻する。
    info.content_height = std::max(0.0f, rect.height - header_height);
    info.total_content = total_content;
    info.max_scroll = std::max(0.0f, total_content - info.content_height);
    const float thumb_ratio = (total_content > 0) ? info.content_height / total_content : 1.0f;
    info.thumb_height = std::min(info.content_height, std::max(thumb_min, info.content_height * thumb_ratio));
    return info;
}

float ComputeThumbY(const PaneScrollInfo& info, float scroll_y) noexcept
{
    const float scroll_ratio = (info.max_scroll > 0) ? scroll_y / info.max_scroll : 0.0f;
    return info.content_top + scroll_ratio * (info.content_height - info.thumb_height);
}

float ScrollFromThumbY(const PaneScrollInfo& info, float thumb_y) noexcept
{
    const float track_range = info.content_height - info.thumb_height;
    float ratio = (track_range > 0) ? (thumb_y - info.content_top) / track_range : 0.0f;
    ratio = std::clamp(ratio, 0.0f, 1.0f);
    return ratio * info.max_scroll;
}
