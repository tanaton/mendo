#pragma once
#include <windows.h>
#include <cstdint>
#include "search_bar_controller.h"
#include "resource_manager.h"
#include "mermaid.h"

// App とWin32Window 間で共有されるタイマーID・カスタムメッセージ定数。
// 各IDは一意であり、変更する際は全IDの重複がないことを確認すること。
// サブコンポーネントが定義する定数から導出し、値の二重管理を防ぐ。
namespace app_timer {

inline constexpr UINT_PTR DEFERRED_LAYOUT = 3;
inline constexpr UINT_PTR LOADING_ANIM = 4;
inline constexpr UINT_PTR SWIPE_OVERLAY = 5;
inline constexpr UINT_PTR TOAST = 6;
inline constexpr UINT_PTR SEARCH_CARET = SearchBarController::TIMER_CARET;
inline constexpr UINT_PTR TOOLTIP = 8;
inline constexpr UINT_PTR SEARCH_DEBOUNCE = SearchBarController::TIMER_DEBOUNCE;
inline constexpr UINT_PTR MERMAID_BATCH = ResourceManager::TIMER_MERMAID_BATCH;
inline constexpr UINT_PTR BITMAP_MANAGE = ResourceManager::TIMER_BITMAP_MANAGE;
inline constexpr UINT_PTR MERMAID_INIT_RETRY = MermaidRenderer::TIMER_INIT_RETRY;
inline constexpr UINT_PTR FILE_RELOAD_DEBOUNCE = 13;

} // namespace app_timer

namespace app_msg {

inline constexpr UINT LOAD_FILE = WM_APP + 1;
inline constexpr UINT IMAGE_LOADED = WM_APP + 2;
inline constexpr UINT RELOAD_FILE = WM_APP + 3;
inline constexpr UINT SEARCH_FOCUS = WM_APP + 4;
inline constexpr UINT SEARCH_UNFOCUS = WM_APP + 5;
inline constexpr UINT PARSE_COMPLETE = WM_APP + 6;

} // namespace app_msg

namespace app_param {

inline constexpr WPARAM SEARCH_FOCUS_SELECT_ALL = 0;
inline constexpr WPARAM SEARCH_FOCUS_SET_CARET = 1;
inline constexpr WPARAM SEARCH_FOCUS_SET_SELECTION = 2; // lParam = MAKELPARAM(anchor, caret)
inline constexpr WPARAM SEARCH_UNFOCUS_CLOSE = 0;
inline constexpr WPARAM SEARCH_UNFOCUS_FILE_SWITCH = 1;

} // namespace app_param
