#pragma once
#include <windows.h>

// Win32 カスタムメッセージID・パラメータ。

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
