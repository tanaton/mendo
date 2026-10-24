#include "pane_controller.h"
#include "ui_constants.h"
#include <algorithm>

namespace {

float ConstrainSplitterWidth(float requested_width, float total_width, float splitter_w, float other_width, bool other_visible) noexcept
{
    // MD ペインの最小幅を残せる上限。ただし SIDE_PANE_MIN_WIDTH を下回らせない。
    const float others = splitter_w + (other_visible ? other_width + splitter_w : 0.0f);
    const float max_w = total_width - MD_PANE_MIN_WIDTH - others;
    return std::max(SIDE_PANE_MIN_WIDTH, std::min(requested_width, max_w));
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
    auto& file = Inst(PaneTarget::File);
    auto& toc = Inst(PaneTarget::Toc);
    const auto layout = ComputeLayout(total_width, 0.0f, splitter_w);
    // ウィンドウが保存幅より狭く表示幅だけ縮小されている間は、見えている幅を論理幅として確定させる。
    // 論理幅のまま制約すると、触った瞬間に縮小が解けて反対側のペインが元の幅へジャンプする。
    const auto adopt_displayed = [](Instance& inst, float displayed) static noexcept {
        if (inst.show && displayed < inst.width) {
            inst.width = std::max(displayed, SIDE_PANE_MIN_WIDTH);
        }
    };
    adopt_displayed(file, layout.file_rect.width);
    adopt_displayed(toc, layout.toc_rect.width);
    if (edge == PaneTarget::File) {
        file.width = ConstrainSplitterWidth(dip_x, total_width, splitter_w, toc.width, toc.show);
    }
    else {
        // dip_x は新しい右端
        toc.width = ConstrainSplitterWidth(dip_x - layout.toc_rect.x, total_width, splitter_w, file.width, file.show);
    }
}

PaneLayout PaneController::ComputeLayout(float total_w, float total_h, float splitter_w, float top_offset) const noexcept
{
    const auto& file = Inst(PaneTarget::File);
    const auto& toc = Inst(PaneTarget::Toc);
    return ComputePaneLayout(total_w, total_h, file.width, toc.width, splitter_w, file.show, toc.show, top_offset);
}

