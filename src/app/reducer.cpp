#include "reducer.h"
#include "ui_constants.h"
#include "utility.h"

SideEffectList Reduce(AppState& state, const AppAction& action)
{
    SideEffectList effects;

    // スクロール位置が変化した場合の共通副作用を発行する
    const auto emit_scroll_effects = [&](float old_scroll) {
        if (state.viewport.GetScrollY() != old_scroll) {
            state.hover_throttle.Reset();
            effects.emplace_back(effect::InvalidateWindow{});
            effects.emplace_back(effect::BitmapManage{});
        }
    };

    std::visit(overloaded{
        [](const NoOpAction&) {},

        // ==== スクロール系 ====
        [&](const KeyScrollAction& a) {
            state.scroll_restore.pending_restore_scroll_y = -1;
            const float old_scroll = state.viewport.GetScrollY();
            const float page_size = state.cached_pane_layout.md_rect.height;
            switch (a.type) {
                case ScrollType::LineUp:   state.viewport.DirectScrollBy(-SCROLL_LINE_AMOUNT); break;
                case ScrollType::LineDown: state.viewport.DirectScrollBy(SCROLL_LINE_AMOUNT); break;
                case ScrollType::PageUp:   state.viewport.DirectScrollBy(-page_size * SCROLL_PAGE_FACTOR); break;
                case ScrollType::PageDown: state.viewport.DirectScrollBy(page_size * SCROLL_PAGE_FACTOR); break;
                case ScrollType::Home:     state.viewport.ScrollTo(0.0f); break;
                case ScrollType::End:      state.viewport.ScrollTo(state.viewport.GetMaxScroll()); break;
                default:                   break;
            }
            emit_scroll_effects(old_scroll);
        },
        [&](const DirectScrollByAction& a) {
            state.scroll_restore.pending_restore_scroll_y = -1;
            const float old_scroll = state.viewport.GetScrollY();
            state.viewport.DirectScrollBy(a.delta);
            emit_scroll_effects(old_scroll);
        },

        // ==== 選択系 ====
        [&](const SelectAllAction&) {
            state.viewport.SelectAll(state.doc.GetNodes());
            effects.emplace_back(effect::InvalidateWindow{});
        },
        [&](const ClearSelectionAction&) {
            if (state.search_state.IsVisible()) {
                state.search_state.Hide();
            } else {
                state.viewport.ClearSelection();
            }
            effects.emplace_back(effect::InvalidateWindow{});
        },

        // ==== システムイベント系 ====
        [&](const ActivateAction& a) {
            if (state.window_active != a.active) {
                state.window_active = a.active;
                effects.emplace_back(effect::InvalidateTitleBar{});
            }
            if (!a.active) {
                effects.emplace_back(effect::ClearTooltip{});
            }
        },
        [&](const EnterSizeMoveAction&) {
            state.is_sizing = true;
        },
        [&](const MouseLeaveAction&) {
            effects.emplace_back(effect::ClearTooltip{});
        },

        // ==== 検索系 ====
        [&](const OpenSearchBarAction&) {
            state.search_bar_ctrl.OnOpen(state.doc.GetNodes());
        },
        [&](const CloseSearchBarAction&) {
            state.search_bar_ctrl.OnClose();
        },
        [&](const SearchNextAction&) {
            if (state.search_state.IsVisible()) {
                state.search_bar_ctrl.OnNext();
            } else {
                state.search_bar_ctrl.OnOpen(state.doc.GetNodes());
            }
        },
        [&](const SearchPrevAction&) {
            if (state.search_state.IsVisible()) {
                state.search_bar_ctrl.OnPrev();
            } else {
                state.search_bar_ctrl.OnOpen(state.doc.GetNodes());
            }
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

        // ==== 未処理（App::Dispatch のフォールバックで処理） ====
        [](const auto&) {},
    }, action);

    return effects;
}
