#pragma once
#include <utility>
#include <windows.h>

// 各 ID は一意。変更する際は重複がないことを確認すること。

namespace app_timer {

// Win32 API に渡す際は std::to_underlying でキャスト。
// 先頭のみ明示値。自動インクリメントにより 1 からのギャップなし連番が言語保証され、
// kFirstTimer/kLastTimer の範囲判定の前提が崩れない。
enum class Id : UINT_PTR {
    LOADING_ANIM = 1,
    SWIPE_OVERLAY,
    TOAST,
    TOOLTIP,
    MERMAID_BATCH,
    BITMAP_MANAGE,
    MERMAID_INIT_RETRY,
    FILE_RELOAD_DEBOUNCE,
    MERMAID_IDLE,
    DEVICE_RECREATE_RETRY,
    END, // 番兵。タイマーとしては使わない。新規タイマーはこの直前に追加する。
};

inline constexpr UINT FRAME_INTERVAL_MS = 16;        // ~60fps アニメーション用
inline constexpr UINT LOADING_SPINNER_DELAY_MS = 150; // 読み込みがこれより長引いたらスピナーを出す (すぐ終わる読み込みでちらつかせない)
inline constexpr UINT FILE_RELOAD_DEBOUNCE_MS = 200; // ファイル変更通知のデバウンス
inline constexpr UINT FILE_RELOAD_RETRY_MS = 50;     // truncate→rewrite 検出後の短縮リトライ
inline constexpr UINT BITMAP_MANAGE_DELAY_MS = 150;  // スクロール停止後のビットマップ解放/再構築
inline constexpr UINT TOAST_DURATION_MS = 1300;
inline constexpr UINT TOOLTIP_DELAY_MS = 500;
inline constexpr UINT DEVICE_RECREATE_RETRY_MS = 500; // 描画デバイスの作り直しに失敗した後の再試行間隔

inline constexpr Id kFirstTimer = Id::LOADING_ANIM;
inline constexpr Id kLastTimer = static_cast<Id>(std::to_underlying(Id::END) - 1);

} // namespace app_timer

namespace app_msg {

inline constexpr UINT IMAGE_LOADED = WM_APP + 2;
inline constexpr UINT PARSE_COMPLETE = WM_APP + 6;
inline constexpr UINT MERMAID_DISK_LOADED = WM_APP + 7;
inline constexpr UINT FILE_LISTED = WM_APP + 8;

// 判定に [WM_APP, END) を使う。
inline constexpr UINT END = WM_APP + 9;

} // namespace app_msg
