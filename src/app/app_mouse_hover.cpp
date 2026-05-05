#include "app.h"
#include "app_constants.h"
#include "app_events.h"
#include "app_mouse_helpers.h"
#include "i18n.h"
#include "pane_layout.h"
#include "string_convert.h"
#include "ui_constants.h"

bool App::IsOverMdScrollbar(float dip_x, float dip_y, const PaneLayout& layout) const noexcept
{
    if (!layout_service_) {
        return false;
    }
    const float total_h = layout_service_->GetTotalHeight();
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
        const auto& r = pane_layout.md_rect;
        const auto sbl = ComputeSearchBarLayout(r.x, r.width, r.y + r.height, !state_.search.search_state.GetQuery().empty());
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

    if (new_hover.copy >= 0) {
        SetCursor(cursors_.Hand());
        Dispatch(UpdateTooltipAction{
            TooltipTarget{ TooltipTarget::Zone::CopyButton, i18n::S().tooltip_copy },
            px, py
        });
        return;
    }
    if (new_hover.svg_copy >= 0) {
        SetCursor(cursors_.Hand());
        Dispatch(UpdateTooltipAction{
            TooltipTarget{ TooltipTarget::Zone::SvgCopyButton, i18n::S().tooltip_copy_svg },
            px, py
        });
        return;
    }
    if (new_hover.save >= 0) {
        SetCursor(cursors_.Hand());
        Dispatch(UpdateTooltipAction{
            TooltipTarget{ TooltipTarget::Zone::SaveButton, i18n::S().tooltip_save_image },
            px, py
        });
        return;
    }

    if (ht.TryMarkMoved(ht.last_md_hit_pos, ht.last_md_hit_tick, px, py)) {
        const auto hit = HitTest(px, py);
        const auto link = GetLinkAtHit(hit);
        ht.last_md_cursor_hand = link.has_value();

        TooltipTarget tt;
        if (link.has_value()) {
            tt.zone = TooltipTarget::Zone::MdLink;
            // tt.text は wstring (Win32 ツールチップ表示用) なので UTF-8 → wstring 変換。
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
        state_.view.panes.IsFilePaneVisible(),
        state_.view.panes.IsTocPaneVisible());

    int new_file_hover = -1;
    int new_toc_hover = -1;

    // ペインゾーン外に出たらヘッダーボタンのホバーをリセット（無効化忘れ防止）。
    if (zone != PaneZone::FilePane) {
        bool changed = state_.view.panes.SetFileCloseHovered(false);
        changed |= state_.view.panes.SetFileRefreshHovered(false);
        if (changed) {
            renderer_.InvalidateFilePaneCache();
            InvalidatePane(pane_layout.file_rect);
        }
    }
    if (zone != PaneZone::TocPane && state_.view.panes.SetTocCloseHovered(false)) {
        renderer_.InvalidateTocPaneCache();
        InvalidatePane(pane_layout.toc_rect);
    }

    switch (zone) {
    case PaneZone::Splitter1:
    case PaneZone::Splitter2:
        SetCursor(cursors_.SizeWE());
        Dispatch(UpdateTooltipAction{ TooltipTarget{}, px, py });
        break;
    case PaneZone::FilePane: {
        const float header_h = renderer_.GetTheme().pane_header_height;
        const auto& entries = state_.file_explorer.GetEntries();
        const auto hr = ProcessSidePaneHover(
            dip_x,
            dip_y,
            pane_layout.file_rect,
            header_h,
            renderer_.GetTheme().pane_item_height,
            true,
            state_.view.panes.FileScroll().scroll_y,
            [this](bool v) { return state_.view.panes.SetFileCloseHovered(v); },
            [this](bool v) { return state_.view.panes.SetFileRefreshHovered(v); },
            [this](float y, float h) { return state_.file_explorer.HitTest(y, h); },
            [&](bool close_hit, bool refresh_hit, int idx) -> TooltipTarget {
            if (close_hit) {
                return {
                    TooltipTarget::Zone::FilePaneButton,
                    i18n::S().tooltip_pane_close
                };
            }
            if (refresh_hit) {
                return {
                    TooltipTarget::Zone::FilePaneButton,
                    i18n::S().tooltip_pane_refresh
                };
            }
            if (idx >= 0 && idx < static_cast<int>(entries.size())) {
                return {
                    TooltipTarget::Zone::FilePaneItem,
                    entries[idx].full_path
                };
            }
            return {};
        });
        SetCursor(hr.any_button_hit ? cursors_.Hand() : cursors_.Arrow());
        if (hr.button_changed) {
            renderer_.InvalidateFilePaneCache();
            InvalidatePane(pane_layout.file_rect);
        }
        new_file_hover = hr.hovered_index;
        Dispatch(UpdateTooltipAction{ hr.tooltip, px, py });
        break;
    }
    case PaneZone::TocPane: {
        const float header_h = renderer_.GetTheme().pane_header_height;
        const auto& toc_entries = state_.document.doc.GetToc().GetEntries();
        const auto hr = ProcessSidePaneHover(
            dip_x,
            dip_y,
            pane_layout.toc_rect,
            header_h,
            renderer_.GetTheme().pane_item_height,
            false,
            state_.view.panes.TocScroll().scroll_y,
            [this](bool v) { return state_.view.panes.SetTocCloseHovered(v); },
            [](bool) { return false; },
            [this](float y, float h) { return state_.document.doc.GetToc().HitTest(y, h); },
            [&](bool close_hit, bool, int idx) -> TooltipTarget {
            if (close_hit) {
                return { TooltipTarget::Zone::TocPaneButton, i18n::S().tooltip_pane_close };
            }
            if (idx >= 0 && idx < static_cast<int>(toc_entries.size())) {
                const auto text = state_.document.doc.GetNodes()[toc_entries[idx].node_index].GetText();
                std::pmr::wstring text_wide;
                string_convert::Utf8ToWide(text, text_wide);
                return { TooltipTarget::Zone::TocPaneItem, text_wide };
            }
            return {};
        });
        SetCursor(hr.any_button_hit ? cursors_.Hand() : cursors_.Arrow());
        if (hr.button_changed) {
            renderer_.InvalidateTocPaneCache();
            InvalidatePane(pane_layout.toc_rect);
        }
        new_toc_hover = hr.hovered_index;
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

    if (state_.view.panes.SetHoveredFileIndex(new_file_hover)) {
        renderer_.InvalidateFilePaneCache();
        InvalidatePane(pane_layout.file_rect);
    }
    if (state_.view.panes.SetHoveredTocIndex(new_toc_hover)) {
        renderer_.InvalidateTocPaneCache();
        InvalidatePane(pane_layout.toc_rect);
    }
}
