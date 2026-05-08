#pragma once
#include <windows.h>

// Reducer・Win32Window・副作用エグゼキュータが共有する Win32 メッセージ／タイマー
// 関連の定数。レンダラー非依存に保ち、include 連鎖を浅く保つ。
// 各 ID は一意であり、変更する際は全 ID の重複がないことを確認すること。

namespace app_timer {

inline constexpr UINT_PTR DEFERRED_LAYOUT = 1;
inline constexpr UINT_PTR LOADING_ANIM = 2;
inline constexpr UINT_PTR SWIPE_OVERLAY = 3;
inline constexpr UINT_PTR TOAST = 4;
inline constexpr UINT_PTR SEARCH_CARET = 5;
inline constexpr UINT_PTR TOOLTIP = 6;
inline constexpr UINT_PTR SEARCH_DEBOUNCE = 7;
inline constexpr UINT_PTR MERMAID_BATCH = 8;
inline constexpr UINT_PTR BITMAP_MANAGE = 9;
inline constexpr UINT_PTR MERMAID_INIT_RETRY = 10;
inline constexpr UINT_PTR FILE_RELOAD_DEBOUNCE = 11;

// タイマー間隔 (ms)
inline constexpr UINT FRAME_INTERVAL_MS = 16;        // ~60fps アニメーション用
inline constexpr UINT FILE_RELOAD_DEBOUNCE_MS = 200; // ファイル変更通知のデバウンス
inline constexpr UINT FILE_RELOAD_RETRY_MS = 50;     // truncate→rewrite 検出後の短縮リトライ

// アプリ終了時に KillTimer する全タイマー ID。Single Source of Truth として
// 上記 ID 定数の定義漏れ・更新漏れを防ぐ。新規タイマーを足したらここに追加すること。
inline constexpr UINT_PTR ALL_TIMERS[] = {
    DEFERRED_LAYOUT,
    LOADING_ANIM,
    SWIPE_OVERLAY,
    TOAST,
    SEARCH_CARET,
    SEARCH_DEBOUNCE,
    TOOLTIP,
    MERMAID_BATCH,
    BITMAP_MANAGE,
    MERMAID_INIT_RETRY,
    FILE_RELOAD_DEBOUNCE,
};

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

namespace app_threshold {

// 同期ロードの上限。これを超えるサイズはバックグラウンドスレッドにオフロードする。
// I/O + パースの典型コストが UI スレッドで <16ms に収まる経験値。
inline constexpr DWORD ASYNC_LOAD_BYTES = 64 * 1024;

// ローディングアニメーション (スピナー) を表示する下限。
// これより小さいファイルはアニメーション表示前に読み終わり、ちらつきの原因になる。
inline constexpr DWORD LOADING_ANIM_BYTES = 16 * 1024 * 1024;

} // namespace app_threshold
