#pragma once
#include <windows.h>

// Reducer・Win32Window・副作用エグゼキュータが共有する Win32 メッセージ／タイマー
// 関連の定数。レンダラー非依存に保ち、include 連鎖を浅く保つ。
// 各 ID は一意であり、変更する際は全 ID の重複がないことを確認すること。

namespace app_timer {

inline constexpr UINT_PTR DEFERRED_LAYOUT = 3;
inline constexpr UINT_PTR LOADING_ANIM = 4;
inline constexpr UINT_PTR SWIPE_OVERLAY = 5;
inline constexpr UINT_PTR TOAST = 6;
inline constexpr UINT_PTR SEARCH_CARET = 7;
inline constexpr UINT_PTR TOOLTIP = 8;
inline constexpr UINT_PTR SEARCH_DEBOUNCE = 9;
inline constexpr UINT_PTR MERMAID_BATCH = 10;
inline constexpr UINT_PTR BITMAP_MANAGE = 11;
inline constexpr UINT_PTR MERMAID_INIT_RETRY = 12;
inline constexpr UINT_PTR FILE_RELOAD_DEBOUNCE = 13;

// タイマー間隔 (ms)
inline constexpr UINT FRAME_INTERVAL_MS = 16;        // ~60fps アニメーション用
inline constexpr UINT FILE_RELOAD_DEBOUNCE_MS = 200; // ファイル変更通知のデバウンス
inline constexpr UINT FILE_RELOAD_RETRY_MS = 50;     // truncate→rewrite 検出後の短縮リトライ

} // namespace app_timer

namespace app_msg {

inline constexpr UINT LOAD_FILE = WM_APP + 1;
inline constexpr UINT IMAGE_LOADED = WM_APP + 2;
inline constexpr UINT RELOAD_FILE = WM_APP + 3;
inline constexpr UINT SEARCH_FOCUS = WM_APP + 4;
inline constexpr UINT SEARCH_UNFOCUS = WM_APP + 5;
inline constexpr UINT PARSE_COMPLETE = WM_APP + 6;

// カスタムメッセージの上限。アプリ独自メッセージかどうかの判定に [WM_APP, END) を使う。
inline constexpr UINT END = WM_APP + 7;

} // namespace app_msg

namespace app_param {

inline constexpr WPARAM SEARCH_FOCUS_SELECT_ALL = 0;
inline constexpr WPARAM SEARCH_FOCUS_SET_CARET = 1;          // lParam = caret (int)
inline constexpr WPARAM SEARCH_FOCUS_SET_SELECTION = 2;      // lParam = SearchSelectionPayload* (heap, 受信側で delete)
inline constexpr WPARAM SEARCH_UNFOCUS_CLOSE = 0;
inline constexpr WPARAM SEARCH_UNFOCUS_FILE_SWITCH = 1;

// SEARCH_FOCUS_SET_SELECTION の lParam で運ぶペイロード。
// 所有権: 発行側で `new` (MakeSearchSelectionLParam)、SEARCH_FOCUS ハンドラで `delete`。
struct SearchSelectionPayload {
    int anchor;
    int caret;
};

inline LPARAM MakeSearchSelectionLParam(int anchor, int caret)
{
    return reinterpret_cast<LPARAM>(new SearchSelectionPayload{ anchor, caret });
}

} // namespace app_param
