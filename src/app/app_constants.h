#pragma once
#include "timer_ids.h"
#include "search_bar_controller.h"
#include "resource_manager.h"
#include "mermaid.h"

// タイマーIDはtimer_ids.hで定義。コンポーネント定数との一致をコンパイル時検証する。
static_assert(app_timer::SEARCH_CARET == SearchBarController::TIMER_CARET);
static_assert(app_timer::SEARCH_DEBOUNCE == SearchBarController::TIMER_DEBOUNCE);
static_assert(app_timer::MERMAID_BATCH == ResourceManager::TIMER_MERMAID_BATCH);
static_assert(app_timer::BITMAP_MANAGE == ResourceManager::TIMER_BITMAP_MANAGE);
static_assert(app_timer::MERMAID_INIT_RETRY == MermaidRenderer::TIMER_INIT_RETRY);

namespace app_msg {

inline constexpr UINT LOAD_FILE = WM_APP + 1;
inline constexpr UINT IMAGE_LOADED = WM_APP + 2;
inline constexpr UINT RELOAD_FILE = WM_APP + 3;
inline constexpr UINT SEARCH_FOCUS = WM_APP + 4;
inline constexpr UINT SEARCH_UNFOCUS = WM_APP + 5;
inline constexpr UINT PARSE_COMPLETE = WM_APP + 6;

// カスタムメッセージの上限（この値未満が有効範囲）
inline constexpr UINT END = WM_APP + 7;

} // namespace app_msg

namespace app_param {

inline constexpr WPARAM SEARCH_FOCUS_SELECT_ALL = 0;
inline constexpr WPARAM SEARCH_FOCUS_SET_CARET = 1;
inline constexpr WPARAM SEARCH_FOCUS_SET_SELECTION = 2; // lParam = MAKELPARAM(anchor, caret)
inline constexpr WPARAM SEARCH_UNFOCUS_CLOSE = 0;
inline constexpr WPARAM SEARCH_UNFOCUS_FILE_SWITCH = 1;

} // namespace app_param
