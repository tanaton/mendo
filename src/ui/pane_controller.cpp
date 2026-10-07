#include "pane_controller.h"
#include <utility>

namespace {

float ConstrainSplitterWidth(float requested_width, float total_width, float splitter_w, float other_width, bool other_visible) noexcept
{
    // MD ペインの最小幅を残せる上限。ただし PANE_MIN_WIDTH を下回らせない。
    const float others = splitter_w + (other_visible ? other_width + splitter_w : 0.0f);
    const float max_w = total_width - MD_PANE_MIN_WIDTH - others;
    return std::max(PaneController::PANE_MIN_WIDTH, std::min(requested_width, max_w));
}

} // namespace

bool PaneController::ScrollSidePaneBy(PaneTarget t, float delta, float max_scroll) noexcept
{
    auto& scroll_y = Inst(t).scroll.scroll_y;
    const float old = scroll_y;
    scroll_y = std::clamp(scroll_y + delta, 0.0f, max_scroll);
    return scroll_y != old;
}

bool PaneController::SetHoveredSideIndex(PaneTarget t, int idx) noexcept
{
    return std::exchange(Inst(t).hovered_index, idx) != idx;
}

bool PaneController::SetSideHoveredButton(PaneTarget t, PaneHeaderButton button) noexcept
{
    return std::exchange(Inst(t).hovered_button, button) != button;
}

void PaneController::DragSplitterTo(DragTarget target, float dip_x, float total_width, float splitter_w) noexcept
{
    if (!IsSplitterDragTarget(target)) {
        return;
    }
    auto& file = Inst(PaneTarget::File);
    auto& toc = Inst(PaneTarget::Toc);
    const auto layout = ComputeLayout(total_width, 0.0f, splitter_w);
    // ウィンドウが保存幅より狭く表示幅だけ縮小されている間は、見えている幅を論理幅として確定させる。
    // 論理幅のまま制約すると、触った瞬間に縮小が解けて反対側のペインが元の幅へジャンプする。
    const auto adopt_displayed = [](Instance& inst, float displayed) noexcept {
        if (inst.show && displayed < inst.width) {
            inst.width = std::max(displayed, PANE_MIN_WIDTH);
        }
    };
    adopt_displayed(file, layout.file_rect.width);
    adopt_displayed(toc, layout.toc_rect.width);
    if (target == DragTarget::Splitter1) {
        file.width = ConstrainSplitterWidth(dip_x, total_width, splitter_w, toc.width, toc.show);
    }
    else {
        // dip_x は新しい右端
        toc.width = ConstrainSplitterWidth(dip_x - layout.toc_rect.x, total_width, splitter_w, file.width, file.show);
    }
}

void PaneController::ApplyZoom(float ratio) noexcept
{
    for (auto& inst : instances_) {
        inst.width *= ratio;
        inst.scroll.scroll_y *= ratio;
    }
}

PaneLayout PaneController::ComputeLayout(float total_w, float total_h, float splitter_w, float top_offset) const noexcept
{
    const auto& file = Inst(PaneTarget::File);
    const auto& toc = Inst(PaneTarget::Toc);
    return ComputePaneLayout(total_w, total_h, file.width, toc.width, splitter_w, file.show, toc.show, MD_PANE_MIN_WIDTH, top_offset);
}

