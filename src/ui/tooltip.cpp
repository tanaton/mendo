#include "tooltip.h"
#include "app_constants.h"
#include "win32_util.h"
#include <commctrl.h>

namespace {
constexpr UINT_PTR TOOL_ID = 1;

// TTF_TRACK ツールを 1 つだけ登録し、全操作でそれを指す。
TOOLINFOW MakeToolInfo(HWND parent) noexcept
{
    TOOLINFOW ti{};
    ti.cbSize = sizeof(ti);
    ti.hwnd = parent;
    ti.uId = TOOL_ID;
    return ti;
}
} // namespace

Tooltip::~Tooltip()
{
    if (hwnd_) {
        DestroyWindow(hwnd_);
    }
}

void Tooltip::Init(HWND parent_hwnd, UINT_PTR timer_id)
{
    parent_ = parent_hwnd;
    timer_id_ = timer_id;
}

// 起動時の初回描画を遅らせないよう、ウィンドウは最初の表示まで作らない。
bool Tooltip::EnsureWindow()
{
    if (hwnd_ || !parent_) {
        return hwnd_ != nullptr;
    }
    hwnd_ = CreateWindowExW(
        WS_EX_TOPMOST,
        TOOLTIPS_CLASSW,
        nullptr,
        WS_POPUP | TTS_NOPREFIX | TTS_ALWAYSTIP,
        CW_USEDEFAULT, CW_USEDEFAULT,
        CW_USEDEFAULT, CW_USEDEFAULT,
        parent_,
        nullptr,
        GetModuleHandleW(nullptr),
        nullptr);
    if (!hwnd_) {
        return false;
    }

    // 位置と表示を手動で制御するため TTF_TRACK で登録する。
    TOOLINFOW ti = MakeToolInfo(parent_);
    ti.uFlags = TTF_TRACK | TTF_ABSOLUTE;
    ti.lpszText = const_cast<LPWSTR>(L"");
    SendMessageW(hwnd_, TTM_ADDTOOLW, 0, reinterpret_cast<LPARAM>(&ti));

    // 長い URL の折り返し用
    SendMessageW(hwnd_, TTM_SETMAXTIPWIDTH, 0, 600);
    if (dark_) {
        ApplyDarkModeToWindow(hwnd_, true);
    }
    return true;
}

void Tooltip::Arm(std::wstring_view text)
{
    Hide();
    text_ = text;
    if (text_.empty()) {
        if (phase_ == Phase::Armed) {
            KillTimer(parent_, timer_id_);
        }
        phase_ = Phase::Hidden;
        return;
    }
    SetTimer(parent_, timer_id_, app_timer::TOOLTIP_DELAY_MS, nullptr);
    phase_ = Phase::Armed;
}

void Tooltip::OnTimer()
{
    KillTimer(parent_, timer_id_);
    if (phase_ == Phase::Armed) {
        Show();
    }
}

void Tooltip::Show()
{
    if (!EnsureWindow()) {
        phase_ = Phase::Hidden;
        return;
    }

    TOOLINFOW ti = MakeToolInfo(parent_);
    ti.lpszText = const_cast<LPWSTR>(text_.c_str());
    SendMessageW(hwnd_, TTM_UPDATETIPTEXTW, 0, reinterpret_cast<LPARAM>(&ti));

    // 表示時点の実カーソル位置を使う。タイマー待機中に同一ターゲット内でカーソルが
    // 動いても最新位置で表示するため (同一ターゲットでは reducer 側で抑止され SetText は
    // 呼ばれないので、保存した位置では陳腐化する)。
    POINT pt{};
    GetCursorPos(&pt);
    const UINT dpi = GetDpiForWindow(parent_);
    const int offset_y = MulDiv(20, dpi, USER_DEFAULT_SCREEN_DPI);
    SendMessageW(hwnd_, TTM_TRACKPOSITION, 0, MAKELPARAM(pt.x, pt.y + offset_y));

    SendMessageW(hwnd_, TTM_TRACKACTIVATE, TRUE, reinterpret_cast<LPARAM>(&ti));
    phase_ = Phase::Shown;
}

void Tooltip::Hide()
{
    if (phase_ != Phase::Shown) {
        return;
    }
    TOOLINFOW ti = MakeToolInfo(parent_);
    SendMessageW(hwnd_, TTM_TRACKACTIVATE, FALSE, reinterpret_cast<LPARAM>(&ti));
    phase_ = Phase::Hidden;
}

void Tooltip::ApplyDarkMode(bool dark)
{
    dark_ = dark;
    if (hwnd_) {
        ApplyDarkModeToWindow(hwnd_, dark);
    }
}
