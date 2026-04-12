#include "reducer.h"
#include "document_utils.h"
#include "ui_constants.h"
#include "utility.h"

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
        const float item_h = state.cached_pane_item_height;
        const float header_h = state.cached_pane_header_height;
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
        [](const auto&) {},
        }, action);

    return effects;
}
