#include "reducer.h"
#include "timer_ids.h"
#include "document_utils.h"
#include "ui_constants.h"
#include "utility.h"

AnchorState SaveAnchorFromState(const AppState& state) noexcept
{
    AnchorState a;
    a.idx = state.viewport.FindFirstVisibleNode(state.layout_cache, state.doc.GetNodes().size());
    a.y_before = (a.idx >= 0) ? state.layout_cache[a.idx].y_position : 0.0f;
    a.offset = state.viewport.GetScrollY() - a.y_before;
    return a;
}

// ナビゲーション結果を副作用に変換するヘルパー。
// NavHistory::GoBack/GoForward の結果に応じて LoadFile or 同一ファイル内スクロールを行う。
static void ApplyNavResult(AppState& state, SideEffectList& effects, NavEntry&& entry)
{
    if (entry.file_path != state.doc.GetFilePath() && !entry.file_path.empty()) {
        // 別ファイルへのナビゲーション
        state.scroll_restore.pending_nav_scroll_y = entry.scroll_y;
        effects.emplace_back(effect::LoadFile{ std::move(entry.file_path) });
    }
    else {
        // 同一ファイル内スクロール
        state.scroll_restore.pending_restore_scroll_y = -1;
        state.viewport.ScrollTo(entry.scroll_y);
        state.hover_throttle.Reset();
        state.tooltip.Hide();
        state.tooltip.ResetTarget();
        effects.emplace_back(effect::ClearTooltip{});
        effects.emplace_back(effect::InvalidateWindow{});
        effects.emplace_back(effect::BitmapManage{});
    }
}

static void ReduceNavigateBack(AppState& state, SideEffectList& effects)
{
    NavEntry out;
    if (state.nav_history.GoBack(NavEntry{ state.doc.GetFilePath(), state.viewport.GetScrollY() }, out)) {
        ApplyNavResult(state, effects, std::move(out));
    }
}

static void ReduceNavigateForward(AppState& state, SideEffectList& effects)
{
    NavEntry out;
    if (state.nav_history.GoForward(NavEntry{ state.doc.GetFilePath(), state.viewport.GetScrollY() }, out)) {
        ApplyNavResult(state, effects, std::move(out));
    }
}

SideEffectList Reduce(AppState& state, const AppAction& action)
{
    SideEffectList effects;

    // ツールチップを非表示にしてタイマーを解除する共通処理
    const auto clear_tooltip = [&]() {
        state.tooltip.Hide();
        state.tooltip.ResetTarget();
        effects.emplace_back(effect::ClearTooltip{});
    };

    // スクロール位置が変化した場合の共通副作用を発行する
    const auto emit_scroll_effects = [&](float old_scroll) {
        if (state.viewport.GetScrollY() != old_scroll) {
            clear_tooltip();
            state.hover_throttle.Reset();
            effects.emplace_back(effect::InvalidateWindow{});
            effects.emplace_back(effect::BitmapManage{});
        }
    };

    // ペインの max_scroll を計算するヘルパー
    const auto calc_pane_max_scroll = [&](PaneZone pane) -> float {
        const float item_h = state.cached_theme.pane_item_height;
        const float header_h = state.cached_theme.pane_header_height;
        if (pane == PaneZone::FilePane) {
            const float total = static_cast<float>(state.file_explorer.GetEntries().size()) * item_h;
            return std::max(0.0f, total - (state.cached_pane_layout.file_rect.height - header_h));
        }
        else {
            const float total = static_cast<float>(state.doc.GetToc().GetEntries().size()) * item_h;
            return std::max(0.0f, total - (state.cached_pane_layout.toc_rect.height - header_h));
        }
    };

    std::visit(overloaded{
        [](const NoOpAction&) {},
        [&](const ScrollPaneAction& a) {
            if (a.pane == PaneZone::FilePane) {
                if (state.panes.ScrollFilePaneBy(a.delta, calc_pane_max_scroll(PaneZone::FilePane))) {
                    effects.emplace_back(effect::InvalidatePaneCache{PaneZone::FilePane});
                    effects.emplace_back(effect::InvalidateWindow{});
                }
            }
            else if (a.pane == PaneZone::TocPane) {
                if (state.panes.ScrollTocPaneBy(a.delta, calc_pane_max_scroll(PaneZone::TocPane))) {
                    effects.emplace_back(effect::InvalidatePaneCache{PaneZone::TocPane});
                    effects.emplace_back(effect::InvalidateWindow{});
                }
            }
        },
        [&](const TogglePaneAction& a) {
            switch (a.target) {
            case PaneTarget::File: state.panes.ToggleFilePane(); break;
            case PaneTarget::Toc:  state.panes.ToggleTocPane();  break;
            }
            state.pane_layout_valid = false;
            effects.emplace_back(effect::RefreshPaneLayout{});
        },
        [&](const KeyScrollAction& a) {
            state.scroll_restore.pending_restore_scroll_y = -1;
            const float old_scroll = state.viewport.GetScrollY();
            const float page_size = state.cached_pane_layout.md_rect.height;
            switch (a.type) {
            case ScrollType::LineUp:
                state.viewport.DirectScrollBy(-SCROLL_LINE_AMOUNT);
                break;
            case ScrollType::LineDown:
                state.viewport.DirectScrollBy(SCROLL_LINE_AMOUNT);
                break;
            case ScrollType::PageUp:
                state.viewport.DirectScrollBy(-page_size * SCROLL_PAGE_FACTOR);
                break;
            case ScrollType::PageDown:
                state.viewport.DirectScrollBy(page_size * SCROLL_PAGE_FACTOR);
                break;
            case ScrollType::Home:
                state.viewport.ScrollTo(0.0f);
                break;
            case ScrollType::End:
                state.viewport.ScrollTo(state.viewport.GetMaxScroll());
                break;
            default:
                break;
            }
            emit_scroll_effects(old_scroll);
        },
        [&](const DirectScrollByAction& a) {
            state.scroll_restore.pending_restore_scroll_y = -1;
            const float old_scroll = state.viewport.GetScrollY();
            state.viewport.DirectScrollBy(a.delta);
            emit_scroll_effects(old_scroll);
        },
        [&](const SelectAllAction&) {
            state.viewport.SelectAll(state.doc.GetNodes());
            effects.emplace_back(effect::InvalidateWindow{});
        },
        [&](const ClearSelectionAction&) {
            if (state.search_state.IsVisible()) {
                state.search_state.Hide();
            }
            else {
                state.viewport.ClearSelection();
            }
            effects.emplace_back(effect::InvalidateWindow{});
        },
        [&](const CopyClipboardAction&) {
            if (state.viewport.GetSelection().active) {
                effects.emplace_back(effect::ClipboardWrite{
                    ExtractSelectedText(state.doc.GetNodes(), state.viewport.GetSelection())});
            }
        },
        [&](const ActivateAction& a) {
            if (state.window_active != a.active) {
                state.window_active = a.active;
                effects.emplace_back(effect::InvalidateTitleBar{});
            }
            if (!a.active) {
                clear_tooltip();
            }
        },
        [&](const EnterSizeMoveAction&) {
            state.is_sizing = true;
        },
        [&](const ExitSizeMoveAction&) {
            state.is_sizing = false;
            effects.emplace_back(effect::PerformResizeEnd{});
        },
        [&](const MouseLeaveAction&) {
            clear_tooltip();
        },
        [&](const OpenSearchBarAction&) {
            state.search_bar_ctrl.OnOpen(state.doc.GetNodes());
        },
        [&](const CloseSearchBarAction&) {
            state.search_bar_ctrl.OnClose();
        },
        [&](const SearchNextAction&) {
            state.search_state.IsVisible()
                ? state.search_bar_ctrl.OnNext()
                : state.search_bar_ctrl.OnOpen(state.doc.GetNodes());
        },
        [&](const SearchPrevAction&) {
            state.search_state.IsVisible()
                ? state.search_bar_ctrl.OnPrev()
                : state.search_bar_ctrl.OnOpen(state.doc.GetNodes());
        },
        [&](const SearchTextChangedAction& a) {
            state.search_bar_ctrl.OnTextChanged(a.text, state.doc.GetNodes());
        },
        [&](const ToggleCaseSensitiveAction&) {
            state.search_bar_ctrl.OnToggleCaseSensitive(state.doc.GetNodes());
        },
        [&](const ToggleHighlightAction&) {
            state.search_bar_ctrl.OnToggleHighlight();
        },
        [&](const SearchSelectionAction& a) {
            state.search_bar_ctrl.SetSelection(a.sel_start, a.sel_end);
        },
        [&](const ImeCompositionAction& a) {
            state.search_bar_ctrl.SetImeComposition(a.text);
        },
        [&](const CaptureChangedAction&) {
            state.search_bar_ctrl.OnCaptureChanged();
            if (state.gesture.GetPhase() != GesturePhase::Idle) {
                state.gesture.Reset();
                effects.emplace_back(effect::InvalidateWindow{});
            }
        },
        [&](const DropFilesAction& a) {
            if (!state.doc.GetFilePath().empty()) {
                state.nav_history.Push(NavEntry{ state.doc.GetFilePath(), state.viewport.GetScrollY() });
            }
            effects.emplace_back(effect::LoadFile{ std::pmr::wstring(a.path) });
        },
        [&](const ShowHelpAction&) {
            if (!state.doc.GetFilePath().empty() && !IsHelpPath(state.doc.GetFilePath())) {
                state.nav_history.Push(NavEntry{ state.doc.GetFilePath(), state.viewport.GetScrollY() });
            }
            effects.emplace_back(effect::LoadFile{ std::pmr::wstring(HELP_PATH) });
        },
        [&](const OpenFileAction&) {
            effects.emplace_back(effect::OpenFileDialog{});
        },
        [&](const ReloadFileAction&) {
            effects.emplace_back(effect::ReloadFile{});
        },
        [&](const FileWatchAction&) {
            effects.emplace_back(effect::CheckFileChanges{});
        },
        [&](const ImageLoadedAction&) {
            effects.emplace_back(effect::NotifyImageLoaded{});
        },
        [&](const NavigateBackAction&) {
            ReduceNavigateBack(state, effects);
        },
        [&](const NavigateForwardAction&) {
            ReduceNavigateForward(state, effects);
        },
        [&](const XButtonBackAction&) {
            ReduceNavigateBack(state, effects);
        },
        [&](const XButtonForwardAction&) {
            ReduceNavigateForward(state, effects);
        },
        [&](const ZoomAction& a) {
            float new_zoom = 0.0f;
            switch (a.direction) {
            case ZoomDirection::In:
                new_zoom = state.viewport.ZoomIn();
                break;
            case ZoomDirection::Out:
                new_zoom = state.viewport.ZoomOut();
                break;
            case ZoomDirection::Reset:
                new_zoom = state.viewport.ZoomReset();
                break;
            }
            if (new_zoom <= 0.0f) {
                return;
            }
            // SaveAnchor はレイアウト無効化の前に実行
            const auto anchor = SaveAnchorFromState(state);
            state.pane_layout_valid = false;
            const float zoom_ratio = new_zoom / state.cached_theme.zoom;
            state.panes.ApplyZoom(zoom_ratio);
            state.layout_cache.InvalidateAllLayouts();
            effects.emplace_back(effect::ApplyThemeChange{
                .type = effect::ApplyThemeChange::Type::Zoom,
                .anchor_idx = anchor.idx,
                .anchor_y_before = anchor.y_before,
                .anchor_offset = anchor.offset,
                .offset_scale = zoom_ratio,
                .new_zoom = new_zoom,
                .zoom_index = state.viewport.GetZoomIndex(),
            });
        },
        [&](const ToggleDarkModeAction&) {
            const auto anchor = SaveAnchorFromState(state);
            state.pane_layout_valid = false;
            state.layout_cache.InvalidateAllWithDiagrams(state.doc.GetNodes());
            effects.emplace_back(effect::ApplyThemeChange{
                .type = effect::ApplyThemeChange::Type::DarkMode,
                .anchor_idx = anchor.idx,
                .anchor_y_before = anchor.y_before,
                .anchor_offset = anchor.offset,
                .offset_scale = 1.0f,
                .new_zoom = 0.0f,
                .zoom_index = state.viewport.GetZoomIndex(),
            });
        },
        [&](const ResizeAction& a) {
            if (a.width == 0 || a.height == 0) {
                return;
            }
            state.pane_layout_valid = false;
            effects.emplace_back(effect::RendererResize{ a.width, a.height });
            const float window_w_dip = a.width / state.cached_dpi_scale;
            state.titlebar.UpdateLayout(window_w_dip);
            if (state.is_sizing) {
                // sizing中は簡易更新: RendererResize 後に PaneLayout 再計算 → max_scroll 同期
                effects.emplace_back(effect::PerformSizingUpdate{});
            }
            else {
                effects.emplace_back(effect::PerformResizeEnd{});
            }
        },
        [&](const DpiChangedAction& a) {
            state.cached_dpi_scale = static_cast<float>(a.dpi) / 96.0f;
            if (state.cached_dpi_scale <= 0.0f) {
                state.cached_dpi_scale = 1.0f;
            }
            state.pane_layout_valid = false;
            state.layout_cache.MarkAllDirty();
            effects.emplace_back(effect::RendererSetDpi{ static_cast<float>(a.dpi) });
            effects.emplace_back(effect::ClearFileCache{});
            effects.emplace_back(effect::SetWindowPosition{
                static_cast<int>(a.suggested.left),
                static_cast<int>(a.suggested.top),
                static_cast<int>(a.suggested.right - a.suggested.left),
                static_cast<int>(a.suggested.bottom - a.suggested.top),
            });
        },
        [&](const HWheelAction& a) {
            const bool had_overlay = state.swipe_detector.IsOverlayVisible();
            const int old_direction = state.swipe_detector.GetOverlayDirection();
            state.swipe_detector.OnHWheel(a.delta, a.tick);
            effects.emplace_back(effect::SetTimer{ app_timer::SWIPE_OVERLAY,
                static_cast<UINT>(SwipeDetector::COMMIT_TIMEOUT_MS) });
            if (had_overlay != state.swipe_detector.IsOverlayVisible()
                || old_direction != state.swipe_detector.GetOverlayDirection()) {
                effects.emplace_back(effect::InvalidateWindow{});
            }
        },
        [&](const LButtonDownAction& a) {
            effects.emplace_back(effect::HandleMouseEvent{ effect::MouseEventType::LButtonDown, a.px, a.py });
        },
        [&](const LButtonUpAction& a) {
            effects.emplace_back(effect::HandleMouseEvent{ effect::MouseEventType::LButtonUp, a.px, a.py });
        },
        [&](const MouseMoveAction& a) {
            effects.emplace_back(effect::HandleMouseEvent{ effect::MouseEventType::MouseMove, a.px, a.py });
        },
        [&](const MouseHoverAction& a) {
            effects.emplace_back(effect::HandleMouseEvent{ effect::MouseEventType::MouseHover, a.px, a.py });
        },
        [&](const LButtonDblClkAction& a) {
            effects.emplace_back(effect::HandleMouseEvent{ effect::MouseEventType::LButtonDblClk, a.px, a.py });
        },
        [&](const RButtonDownAction& a) {
            effects.emplace_back(effect::HandleMouseEvent{ effect::MouseEventType::RButtonDown, a.px, a.py });
        },
        [&](const RButtonUpAction& a) {
            effects.emplace_back(effect::HandleMouseEvent{ effect::MouseEventType::RButtonUp, a.px, a.py });
        },
        [&](const RButtonMoveAction& a) {
            effects.emplace_back(effect::HandleMouseEvent{ effect::MouseEventType::RButtonMove, a.px, a.py });
        },
        [&](const ContextMenuAction& a) {
            effects.emplace_back(effect::HandleContextMenu{ a.screen_x, a.screen_y });
        },
        [&](const TimerAction& a) {
            switch (a.timer_id) {
            case app_timer::TOAST:
                if (!state.toast.Tick()) {
                    effects.emplace_back(effect::KillTimer{ app_timer::TOAST });
                }
                effects.emplace_back(effect::InvalidateWindow{});
                break;
            case app_timer::SEARCH_CARET:
                state.search_bar_ctrl.OnCaretBlinkTimer();
                break;
            case app_timer::TOOLTIP:
                effects.emplace_back(effect::KillTimer{ app_timer::TOOLTIP });
                state.tooltip.Show();
                break;
            case app_timer::SEARCH_DEBOUNCE:
                state.search_bar_ctrl.OnDebounceTimer(state.doc.GetNodes());
                break;
            case app_timer::SWIPE_OVERLAY: {
                const auto result = state.swipe_detector.Commit();
                effects.emplace_back(effect::KillTimer{ app_timer::SWIPE_OVERLAY });
                switch (result) {
                case SwipeResult::Back:
                    ReduceNavigateBack(state, effects);
                    effects.emplace_back(effect::InvalidateWindow{});
                    break;
                case SwipeResult::Forward:
                    ReduceNavigateForward(state, effects);
                    effects.emplace_back(effect::InvalidateWindow{});
                    break;
                default:
                    break;
                }
                break;
            }
            case app_timer::DEFERRED_LAYOUT:
                effects.emplace_back(effect::ProcessDeferredLayout{});
                break;
            case app_timer::LOADING_ANIM:
                effects.emplace_back(effect::TickLoadingAnimation{});
                effects.emplace_back(effect::InvalidateWindow{});
                break;
            case app_timer::MERMAID_BATCH:
                effects.emplace_back(effect::ProcessMermaidBatchTimer{});
                break;
            case app_timer::BITMAP_MANAGE:
                effects.emplace_back(effect::ProcessBitmapManage{});
                break;
            case app_timer::MERMAID_INIT_RETRY:
                effects.emplace_back(effect::MermaidInitRetry{});
                break;
            case app_timer::FILE_RELOAD_DEBOUNCE:
                effects.emplace_back(effect::KillTimer{ app_timer::FILE_RELOAD_DEBOUNCE });
                effects.emplace_back(effect::ReloadFile{});
                break;
            default:
                break;
            }
        },
        [&](const DestroyAction&) {
            effects.emplace_back(effect::Destroy{});
        },
        [&](const ParseCompleteAction&) {
            effects.emplace_back(effect::HandleParseComplete{});
        },
        [](const auto&) {},
        }, action);

    return effects;
}
