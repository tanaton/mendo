#pragma once
#include <cstdint>
#include <utility>
#include <windows.h>

// 各 ID は一意。変更する際は重複がないことを確認すること。

namespace app_timer {

// Win32 API に渡す際は std::to_underlying でキャスト。
enum class Id : UINT_PTR {
    DEFERRED_LAYOUT = 1,
    LOADING_ANIM = 2,
    SWIPE_OVERLAY = 3,
    TOAST = 4,
    SEARCH_CARET = 5,
    TOOLTIP = 6,
    SEARCH_DEBOUNCE = 7,
    MERMAID_BATCH = 8,
    BITMAP_MANAGE = 9,
    MERMAID_INIT_RETRY = 10,
    FILE_RELOAD_DEBOUNCE = 11,
};

inline constexpr UINT FRAME_INTERVAL_MS = 16;        // ~60fps アニメーション用
inline constexpr UINT FILE_RELOAD_DEBOUNCE_MS = 200; // ファイル変更通知のデバウンス
inline constexpr UINT FILE_RELOAD_RETRY_MS = 50;     // truncate→rewrite 検出後の短縮リトライ

// 新規タイマーを足したらここに追加すること。
inline constexpr Id ALL_TIMERS[] = {
    Id::DEFERRED_LAYOUT,
    Id::LOADING_ANIM,
    Id::SWIPE_OVERLAY,
    Id::TOAST,
    Id::SEARCH_CARET,
    Id::SEARCH_DEBOUNCE,
    Id::TOOLTIP,
    Id::MERMAID_BATCH,
    Id::BITMAP_MANAGE,
    Id::MERMAID_INIT_RETRY,
    Id::FILE_RELOAD_DEBOUNCE,
};

} // namespace app_timer

namespace app_msg {

inline constexpr UINT IMAGE_LOADED = WM_APP + 2;
inline constexpr UINT SEARCH_FOCUS = WM_APP + 4;
inline constexpr UINT SEARCH_UNFOCUS = WM_APP + 5;
inline constexpr UINT PARSE_COMPLETE = WM_APP + 6;

// 判定に [WM_APP, END) を使う。
inline constexpr UINT END = WM_APP + 7;

} // namespace app_msg

namespace app_param {

inline constexpr WPARAM SEARCH_FOCUS_SELECT_ALL = 0;
inline constexpr WPARAM SEARCH_FOCUS_SET_CARET = 1;     // lParam = caret (int)
inline constexpr WPARAM SEARCH_FOCUS_SET_SELECTION = 2; // lParam = (anchor << 32) | caret (int x 2)
inline constexpr WPARAM SEARCH_UNFOCUS_CLOSE = 0;
inline constexpr WPARAM SEARCH_UNFOCUS_FILE_SWITCH = 1;

// 64bit LPARAM (x64/arm64 ビルド前提) に int × 2 を載せて動的確保を排除。
// PostMessage 成功後に hwnd 破棄が起きても OS の message queue に残るのは値だけで leak しない。
static_assert(sizeof(LPARAM) >= sizeof(uint64_t), "x64/arm64 build expected (LPARAM must hold int x 2)");

inline LPARAM MakeSearchSelectionLParam(int anchor, int caret) noexcept
{
    const auto a = static_cast<uint64_t>(static_cast<uint32_t>(anchor));
    const auto c = static_cast<uint64_t>(static_cast<uint32_t>(caret));
    return static_cast<LPARAM>((a << 32) | c);
}

inline std::pair<int, int> UnpackSearchSelectionLParam(LPARAM lp) noexcept
{
    const auto raw = static_cast<uint64_t>(lp);
    return {
        static_cast<int>(static_cast<uint32_t>(raw >> 32)),
        static_cast<int>(static_cast<uint32_t>(raw)),
    };
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
