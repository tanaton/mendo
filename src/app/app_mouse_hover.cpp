#include "app.h"
#include "app_constants.h"
#include "app_events.h"
#include "app_mouse_helpers.h"
#include "block_h_scroll.h"
#include "i18n.h"
#include "layout_computer.h"
#include "pane_layout.h"
#include "string_convert.h"
#include "ui_constants.h"

bool App::IsOverMdScrollbar(float dip_x, float dip_y, const PaneLayout& layout) const noexcept
{
    const float total_h = ScrollableContentHeight();
    const float viewport_h = layout.md_rect.height;
    if (total_h <= viewport_h || viewport_h <= 0.0f) {
        return false;
    }
    if (dip_y < layout.md_rect.y || dip_y > layout.md_rect.y + viewport_h) {
        return false;
    }
    const float md_right = layout.md_rect.x + layout.md_rect.width;
    const float sb_left = md_right - PANE_SCROLLBAR_WIDTH - PANE_SCROLLBAR_MARGIN;
    const float sb_right = md_right - PANE_SCROLLBAR_MARGIN;
    return dip_x >= sb_left - PANE_SCROLLBAR_HIT_PADDING && dip_x <= sb_right;
}

bool App::IsOverMdScrollbar(float dip_x, float dip_y)
{
    return IsOverMdScrollbar(dip_x, dip_y, GetPaneLayout());
}

void App::HandleMdPaneHover(float dip_x, float dip_y, int px, int py, const PaneLayout& pane_layout)
{
    if (state_.search.search_state.IsVisible()) {
        const auto sbl = ComputeSearchBarLayoutForMd(pane_layout.md_rect);
        const auto old_hover = state_.search.search_bar_ctrl.GetHover();

        if (dip_y >= sbl.bar_top) {
            const auto zone = HitTestSearchBar(sbl, dip_x, dip_y);
            state_.search.search_bar_ctrl.UpdateHoverFromZone(zone);

            SetCursor(zone == SearchBarHitZone::Input ? cursors_.IBeam() : cursors_.Arrow());
            Dispatch(UpdateTooltipAction{ mendo::app_mouse::BuildSearchBarTooltip(zone), px, py });
            return;
        }

        if (old_hover != SearchBarHitZone::None) {
            state_.search.search_bar_ctrl.ResetHover();
            Invalidate();
        }
    }

    if (IsOverMdScrollbar(dip_x, dip_y, pane_layout)) {
        SetCursor(cursors_.Arrow());
        Dispatch(UpdateTooltipAction{ TooltipTarget{}, px, py });
        return;
    }

    const auto nav_hit = hit_test_.NavButtonHitTest(dip_x, dip_y, pane_layout.md_rect);
    Dispatch(MdPaneNavHoverAction{ nav_hit });
    if (nav_hit != NavButtonHover::None) {
        SetCursor(cursors_.Hand());
        Dispatch(UpdateTooltipAction{ mendo::app_mouse::BuildNavButtonTooltip(nav_hit), px, py });
        return;
    }

    // 距離+時間スロットリングで再計算を抑え、1 回の可視ノード走査でコピー/保存/SVG コピー
    // ボタンのホバーを同時に判定する。
    const auto hit_ctx = BuildMdPaneHitContext(px, py, pane_layout);
    auto& ht = state_.interaction.hover_throttle;
    HoveredButtons new_hover = state_.interaction.hovered;
    if (ht.TryMarkMoved(ht.last_copy_hit_pos, ht.last_copy_hit_tick, px, py)) {
        const auto btn_hit = hit_test_.CodeBlockButtonsHitTest(hit_ctx);
        new_hover = { btn_hit.copy_node, btn_hit.save_node, btn_hit.svg_copy_node };
    }
    if (new_hover != state_.interaction.hovered) {
        Dispatch(MdPaneButtonHoverChangedAction{ new_hover });
    }

    const auto emit_button_hover = [&](TooltipTarget::Zone zone, std::wstring_view text) {
        SetCursor(cursors_.Hand());
        Dispatch(UpdateTooltipAction{
            TooltipTarget{ zone, text },
            px, py
        });
    };
    if (new_hover.copy >= 0) {
        emit_button_hover(TooltipTarget::Zone::CopyButton, i18n::S().tooltip_copy);
        return;
    }
    if (new_hover.svg_copy >= 0) {
        emit_button_hover(TooltipTarget::Zone::SvgCopyButton, i18n::S().tooltip_copy_svg);
        return;
    }
    if (new_hover.save >= 0) {
        emit_button_hover(TooltipTarget::Zone::SaveButton, i18n::S().tooltip_save_image);
        return;
    }

    if (ht.TryMarkMoved(ht.last_md_hit_pos, ht.last_md_hit_tick, px, py)) {
        const auto hit = HitTest(hit_ctx);
        const auto link = GetLinkAtHit(hit);
        const bool has_link = link.has_value();
        ht.last_md_cursor_hand = has_link;

        // 横スクロール対象 (Table / CodeBlock) で自然幅 > 可視幅 のときだけバーを出す。
        // ドラッグ中は hovered を固定して、スクロールバー直下に出ても見た目が動かないようにする。
        if (state_.view.h_drag_node < 0) {
            int new_h_block = -1;
            if (hit.node_index >= 0) {
                const auto& nodes = state_.document.doc.GetNodes();
                const auto& cache = state_.document.layout_cache;
                if (hit.node_index < static_cast<int>(nodes.size()) && hit.node_index < static_cast<int>(cache.size())) {
                    const auto geom = GetBlockHScrollGeometry(
                        nodes[hit.node_index], cache[hit.node_index], renderer_.GetTheme(), pane_layout.md_rect.width);
                    if (geom.can_scroll()) {
                        new_h_block = hit.node_index;
                    }
                }
            }
            if (new_h_block != state_.view.hovered_h_block) {
                Dispatch(BlockHHoverChangedAction{ new_h_block });
            }
        }

        TooltipTarget tt;
        if (has_link) {
            tt.zone = TooltipTarget::Zone::MdLink;
            string_convert::Utf8ToWide(*link, tt.text);
        }
        else if (hit.node_index >= 0) {
            const auto& nodes = state_.document.doc.GetNodes();
            const auto& node = nodes[hit.node_index];
            if (auto* const img = node.image_data(); img && node.type == NodeType::Image) {
                tt.zone = TooltipTarget::Zone::MdImage;
                const auto& alt = node.GetText();
                if (!alt.empty()) {
                    string_convert::Utf8ToWide(alt, tt.text);
                    tt.text += L"\n";
                }
                std::pmr::wstring src_wide;
                string_convert::Utf8ToWide(img->src, src_wide);
                tt.text += src_wide;
            }
        }
        Dispatch(UpdateTooltipAction{ tt, px, py });
    }
    SetCursor(ht.last_md_cursor_hand ? cursors_.Hand() : cursors_.IBeam());
}

void App::OnMouseHover(int px, int py)
{
    using mendo::app_mouse::BuildTitleBarTooltip;
    using mendo::app_mouse::ProcessSidePaneHover;

    if (!IsRenderReady()) {
        return;
    }

    // OS から同一座標の WM_MOUSEMOVE が連続して届くことがあるため、
    // 完全同一座標なら後段の zone 判定・ヒットテストを全てスキップする。
    if (state_.interaction.hover_throttle.ShouldSkipSameDispatch(px, py)) {
        return;
    }

    const auto dip = PixelToDip(px, py);
    const float dip_x = dip.x;
    const float dip_y = dip.y;

    if (dip_y < state_.window.titlebar.GetHeight()) {
        const auto tb_zone = state_.window.titlebar.HitTest(dip_x, dip_y);
        SetCursor(cursors_.Arrow());
        if (state_.window.titlebar.SetHovered(tb_zone)) {
            InvalidateTitleBar();
        }
        // サイドペインから直接タイトルバーへ移動すると後段のホバー解除に到達しない
        {
            const auto pane_layout = GetPaneLayout();
            for (auto t : { PaneTarget::File, PaneTarget::Toc }) {
                bool changed = state_.view.panes.SetHoveredSideIndex(t, -1);
                changed |= state_.view.panes.SetSideCloseHovered(t, false);
                if (t == PaneTarget::File) {
                    changed |= state_.view.panes.SetSideRefreshHovered(t, false);
                }
                if (changed) {
                    renderer_.InvalidateSidePaneCache(t);
                    InvalidatePane(pane_layout.Get(t));
                }
            }
        }
        Dispatch(UpdateTooltipAction{ BuildTitleBarTooltip(tb_zone, IsZoomed(hwnd_)), px, py });
        return;
    }
    if (state_.window.titlebar.SetHovered(TitleBarHitZone::None)) {
        InvalidateTitleBar();
    }

    const auto pane_layout = GetPaneLayout();
    const auto zone = DetectPaneZone(
        dip_x,
        pane_layout,
        renderer_.GetTheme().splitter_width,
        state_.view.panes.IsSidePaneVisible(PaneTarget::File),
        state_.view.panes.IsSidePaneVisible(PaneTarget::Toc));

    const auto hovered_target = ToPaneTarget(zone);

    // ペインゾーン外に出たらヘッダーボタンのホバーをリセット（無効化忘れ防止）。
    auto reset_pane_buttons = [&](PaneTarget t) {
        bool changed = state_.view.panes.SetSideCloseHovered(t, false);
        if (t == PaneTarget::File) {
            changed |= state_.view.panes.SetSideRefreshHovered(t, false);
        }
        if (changed) {
            renderer_.InvalidateSidePaneCache(t);
            InvalidatePane(pane_layout.Get(t));
        }
    };
    if (hovered_target != PaneTarget::File) {
        reset_pane_buttons(PaneTarget::File);
    }
    if (hovered_target != PaneTarget::Toc) {
        reset_pane_buttons(PaneTarget::Toc);
    }

    int new_hover[2] = { -1, -1 };

    switch (zone) {
    case PaneZone::Splitter1:
    case PaneZone::Splitter2:
        SetCursor(cursors_.SizeWE());
        Dispatch(UpdateTooltipAction{ TooltipTarget{}, px, py });
        break;
    case PaneZone::FilePane:
    case PaneZone::TocPane: {
        const PaneTarget target = *hovered_target;
        const bool is_file = target == PaneTarget::File;
        const auto tooltip = [&](bool close_hit, bool refresh_hit, int idx) -> TooltipTarget {
            if (close_hit) {
                return {
                    is_file ? TooltipTarget::Zone::FilePaneButton : TooltipTarget::Zone::TocPaneButton,
                    i18n::S().tooltip_pane_close
                };
            }
            if (refresh_hit) {
                return {
                    TooltipTarget::Zone::FilePaneButton,
                    i18n::S().tooltip_pane_refresh
                };
            }
            if (is_file) {
                const auto& entries = state_.file_explorer.GetEntries();
                if (idx >= 0 && idx < static_cast<int>(entries.size())) {
                    return { TooltipTarget::Zone::FilePaneItem, entries[idx].full_path };
                }
            }
            else {
                const auto& toc_entries = state_.document.doc.GetToc().GetEntries();
                if (idx >= 0 && idx < static_cast<int>(toc_entries.size())) {
                    // 同一 TOC 項目のホバー継続中は reducer 側で no-op になるため
                    // UTF-8→UTF-16 変換を省く (移動 1 回ごとの pmr::wstring 確保を回避)。
                    const auto& current = state_.interaction.tooltip.GetCurrent();
                    if (idx == state_.view.panes.GetHoveredSideIndex(PaneTarget::Toc) && current.zone == TooltipTarget::Zone::TocPaneItem) {
                        return current;
                    }
                    const auto text = state_.document.doc.GetNodes()[toc_entries[idx].node_index].GetText();
                    std::pmr::wstring text_wide;
                    string_convert::Utf8ToWide(text, text_wide);
                    return { TooltipTarget::Zone::TocPaneItem, text_wide };
                }
            }
            return {};
        };
        // clang-format off
        const auto hr = ProcessSidePaneHover(
            dip_x,
            dip_y,
            pane_layout.Get(target),
            renderer_.GetTheme().pane_header_height,
            renderer_.GetTheme().pane_item_height,
            is_file,
            state_.view.panes.SidePaneScroll(target).scroll_y,
            [this, target](bool v) noexcept {
                return state_.view.panes.SetSideCloseHovered(target, v);
            },
            [this, target](bool v) noexcept {
                return state_.view.panes.SetSideRefreshHovered(target, v);
            },
            [this, is_file](float y, float h) noexcept {
                return is_file
                    ? state_.file_explorer.HitTest(y, h)
                    : state_.document.doc.GetToc().HitTest(y, h);
            },
            tooltip
        );
        // clang-format on
        SetCursor(hr.any_button_hit ? cursors_.Hand() : cursors_.Arrow());
        if (hr.button_changed) {
            renderer_.InvalidateSidePaneCache(target);
            InvalidatePane(pane_layout.Get(target));
        }
        new_hover[static_cast<size_t>(target)] = hr.hovered_index;
        Dispatch(UpdateTooltipAction{ hr.tooltip, px, py });
        break;
    }
    case PaneZone::MdPane:
        HandleMdPaneHover(dip_x, dip_y, px, py, pane_layout);
        break;
    default:
        SetCursor(cursors_.Arrow());
        Dispatch(UpdateTooltipAction{ TooltipTarget{}, px, py });
        break;
    }

    for (auto t : { PaneTarget::File, PaneTarget::Toc }) {
        if (state_.view.panes.SetHoveredSideIndex(t, new_hover[static_cast<size_t>(t)])) {
            renderer_.InvalidateSidePaneCache(t);
            InvalidatePane(pane_layout.Get(t));
        }
    }
}
