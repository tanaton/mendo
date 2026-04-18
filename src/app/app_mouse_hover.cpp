// OnMouseHover のディスパッチ処理。各ペインのホバー処理と、サイドペイン共通ホバー補助を展開する。
#include "app.h"
#include "app_mouse_helpers.h"
#include "i18n.h"
#include "pane_layout.h"

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

    // タイトルバーのホバー処理
    if (dip_y < state_.window.titlebar.GetHeight()) {
        const auto tb_zone = state_.window.titlebar.HitTest(dip_x, dip_y);
        SetCursor(cursors_.Arrow());
        if (state_.window.titlebar.SetHovered(tb_zone)) {
            InvalidateTitleBar();
        }
        UpdateTooltip(BuildTitleBarTooltip(tb_zone, IsZoomed(hwnd_)), px, py);
        return;
    }
    // タイトルバー外に出たらホバーをリセット
    if (state_.window.titlebar.SetHovered(TitleBarHitZone::None)) {
        InvalidateTitleBar();
    }

    const auto pane_layout = GetPaneLayout();
    const auto zone = DetectPaneZone(
        dip_x,
        pane_layout,
        renderer_.GetTheme().splitter_width,
        state_.view.panes.IsFilePaneVisible(),
        state_.view.panes.IsTocPaneVisible()
    );

    int new_file_hover = -1;
    int new_toc_hover = -1;

    // ペインゾーン外に出たら閉じる/更新ボタンのホバーをリセット
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
        UpdateTooltip({}, px, py);
        break;
    case PaneZone::FilePane: {
        const float header_h = renderer_.GetTheme().pane_header_height;
        const auto& entries = state_.file_explorer.GetEntries();
        const auto hr = ProcessSidePaneHover(dip_x, dip_y,
            pane_layout.file_rect, header_h, renderer_.GetTheme().pane_item_height,
            true, state_.view.panes.FileScroll().scroll_y,
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
        UpdateTooltip(hr.tooltip, px, py);
        break;
    }
    case PaneZone::TocPane: {
        const float header_h = renderer_.GetTheme().pane_header_height;
        const auto& toc_entries = state_.document.doc.GetToc().GetEntries();
        const auto hr = ProcessSidePaneHover(dip_x, dip_y,
            pane_layout.toc_rect, header_h, renderer_.GetTheme().pane_item_height,
            false, state_.view.panes.TocScroll().scroll_y,
            [this](bool v) { return state_.view.panes.SetTocCloseHovered(v); },
            [](bool) { return false; },
            [this](float y, float h) { return state_.document.doc.GetToc().HitTest(y, h); },
            [&](bool close_hit, bool, int idx) -> TooltipTarget {
            if (close_hit) { return { TooltipTarget::Zone::TocPaneButton, i18n::S().tooltip_pane_close }; }
            if (idx >= 0 && idx < static_cast<int>(toc_entries.size())) {
                return { TooltipTarget::Zone::TocPaneItem, state_.document.doc.GetNodes()[toc_entries[idx].node_index].GetText() };
            }
            return {};
        });
        SetCursor(hr.any_button_hit ? cursors_.Hand() : cursors_.Arrow());
        if (hr.button_changed) {
            renderer_.InvalidateTocPaneCache();
            InvalidatePane(pane_layout.toc_rect);
        }
        new_toc_hover = hr.hovered_index;
        UpdateTooltip(hr.tooltip, px, py);
        break;
    }
    case PaneZone::MdPane:
        HandleMdPaneHover(dip_x, dip_y, px, py, pane_layout);
        break;
    default:
        SetCursor(cursors_.Arrow());
        UpdateTooltip({}, px, py);
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
