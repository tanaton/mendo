#pragma once
#include <windows.h>

// Reducer と App/Win32Window の両方から参照されるタイマーID定数。
// レンダラー非依存のヘッダとして分離し、値の二重管理を防ぐ。
// 各IDは一意であり、変更する際は全IDの重複がないことを確認すること。
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
inline constexpr UINT FRAME_INTERVAL_MS = 16;              // ~60fps アニメーション用
inline constexpr UINT FILE_RELOAD_DEBOUNCE_MS = 200;       // ファイル変更通知のデバウンス
inline constexpr UINT FILE_RELOAD_RETRY_MS = 50;           // truncate→rewrite 検出後の短縮リトライ

} // namespace app_timer
