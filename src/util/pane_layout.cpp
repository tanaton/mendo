#include "pane_layout.h"
#include <algorithm>

namespace {

// 表示中のサイドペインに使える幅の合計。MD ペインに MD_PANE_MIN_WIDTH を残し、shown_panes 本の
// スプリッタ (各ペインの右端) を除く。表示の縮小とスプリッタのドラッグの制約が同じ規則を使う。
constexpr float SidePanesAvailableWidth(float total_width, float splitter_w, int shown_panes) noexcept
{
    return total_width - MD_PANE_MIN_WIDTH - splitter_w * static_cast<float>(shown_panes);
}

} // namespace

bool PaneController::ScrollSidePaneBy(PaneTarget t, float delta, float max_scroll) noexcept
{
    auto& scroll_y = Inst(t).scroll.scroll_y;
    const float old = scroll_y;
    scroll_y = std::clamp(scroll_y + delta, 0.0f, max_scroll);
    return scroll_y != old;
}

void PaneController::DragSplitterTo(PaneTarget edge, float dip_x, float total_width, float splitter_w) noexcept
{
    const auto layout = ComputeLayout(total_width, 0.0f, splitter_w);
    // ウィンドウが保存幅より狭く表示幅だけ縮小されている間は、見えている幅を論理幅として確定させる。
    // 論理幅のまま制約すると、触った瞬間に縮小が解けて反対側のペインが元の幅へジャンプする。
    const auto adopt_displayed = [this](PaneTarget t, float displayed) noexcept {
        if (Inst(t).show && displayed < Inst(t).width) {
            SetSidePaneWidth(t, displayed);
        }
    };
    adopt_displayed(PaneTarget::File, layout.file_rect.width);
    adopt_displayed(PaneTarget::Toc, layout.toc_rect.width);
    // MD ペインの最小幅を残せる上限 (SetSidePaneWidth が SIDE_PANE_MIN_WIDTH を下回らせない)。ドラッグしているペインは表示中として数える。
    const auto max_width = [&](PaneTarget other_target) noexcept {
        const Instance& other = Inst(other_target);
        return SidePanesAvailableWidth(total_width, splitter_w, other.show ? 2 : 1) - (other.show ? other.width : 0.0f);
    };
    if (edge == PaneTarget::File) {
        SetSidePaneWidth(PaneTarget::File, std::min(dip_x, max_width(PaneTarget::Toc)));
    }
    else {
        // dip_x は新しい右端
        SetSidePaneWidth(PaneTarget::Toc, std::min(dip_x - layout.toc_rect.x, max_width(PaneTarget::File)));
    }
}

PaneLayout PaneController::ComputeLayout(float total_width, float total_height, float splitter_width, float top_offset) const noexcept
{
    const auto& file = Inst(PaneTarget::File);
    const auto& toc = Inst(PaneTarget::Toc);
    PaneLayout layout{ .show_file = file.show, .show_toc = toc.show };
    float x = 0.0f;
    const float pane_height = std::max(total_height - top_offset, 0.0f);

    // 保存幅がウィンドウより広い場合 (広いモニタで保存→狭い画面で起動した等) でも
    // MD ペインとスプリッタを画面内に保つため、表示上の side 幅を clamp する。論理幅
    // (Instance::width) は変更しないので、ウィンドウを広げれば元に戻る。
    // side がウィンドウに収まる通常時は no-op。
    float fw = file.show ? file.width : 0.0f;
    float tw = toc.show ? toc.width : 0.0f;
    const float avail_sides = SidePanesAvailableWidth(total_width, splitter_width, int{ file.show } + int{ toc.show });
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

    if (file.show) {
        layout.file_rect = { x, top_offset, fw, pane_height };
        x += fw + splitter_width;
    }

    if (toc.show) {
        layout.toc_rect = { x, top_offset, tw, pane_height };
        x += tw + splitter_width;
    }

    const float md_width = std::max(MD_PANE_MIN_WIDTH, total_width - x);
    layout.md_rect = { x, top_offset, md_width, pane_height };

    return layout;
}

PaneZone DetectPaneZone(float dip_x, const PaneLayout& layout, float splitter_width) noexcept
{
    // サイドペイン本体と、その右隣のスプリッタを判定する。
    const auto hit_side = [&](const PaneRect& rect, PaneZone pane, PaneZone splitter) noexcept {
        const float splitter_x = rect.x + rect.width;
        if (dip_x >= rect.x && dip_x < splitter_x) {
            return pane;
        }
        if (dip_x >= splitter_x && dip_x < splitter_x + splitter_width) {
            return splitter;
        }
        return PaneZone::None;
    };

    if (layout.show_file) {
        if (const auto zone = hit_side(layout.file_rect, PaneZone::FilePane, PaneZone::Splitter1); zone != PaneZone::None) {
            return zone;
        }
    }
    if (layout.show_toc) {
        if (const auto zone = hit_side(layout.toc_rect, PaneZone::TocPane, PaneZone::Splitter2); zone != PaneZone::None) {
            return zone;
        }
    }
    return dip_x >= layout.md_rect.x ? PaneZone::MdPane : PaneZone::None;
}

PaneScrollInfo ComputeScrollInfo(const PaneRect& rect, float header_height, float total_content) noexcept
{
    PaneScrollInfo info{};
    info.content_top = PaneContentTop(rect, header_height);
    // 負を許すと max_scroll が過大化し thumb_height も負側へ倒れて scrollbar 計算が連鎖破綻する。
    info.content_height = std::max(0.0f, rect.height - header_height);
    info.total_content = total_content;
    info.max_scroll = MaxScroll(total_content, info.content_height);
    const float thumb_ratio = (total_content > 0) ? info.content_height / total_content : 1.0f;
    info.thumb_height = std::min(info.content_height, std::max(PANE_SCROLLBAR_THUMB_MIN, info.content_height * thumb_ratio));
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
    const float ratio = (track_range > 0) ? (thumb_y - info.content_top) / track_range : 0.0f;
    return std::clamp(ratio, 0.0f, 1.0f) * info.max_scroll;
}
