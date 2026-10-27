#include "tooltip.h"
#include <windows.h>
#include <commctrl.h>
#include <dwmapi.h>
#include <uxtheme.h>

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

void ApplyDarkModeToWindow(HWND hwnd, bool dark)
{
    const BOOL value = dark ? TRUE : FALSE;
    DwmSetWindowAttribute(hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE, &value, sizeof(value));

    // エクスプローラーのダークテーマを適用すると非クライアントスクロールバーも
    // ダーク化される（Windows 標準の挙動を借用）。
    SetWindowTheme(hwnd, dark ? L"DarkMode_Explorer" : L"Explorer", nullptr);
}

Tooltip::~Tooltip()
{
    if (hwnd_) {
        DestroyWindow(hwnd_);
    }
}

void Tooltip::Init(HWND parent_hwnd)
{
    parent_ = parent_hwnd;
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

void Tooltip::SetText(std::wstring_view text)
{
    Hide();
    text_ = text;
}

void Tooltip::Show()
{
    if (text_.empty() || !EnsureWindow()) {
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
    visible_ = true;
}

void Tooltip::Hide()
{
    if (!hwnd_ || !visible_) {
        return;
    }
    TOOLINFOW ti = MakeToolInfo(parent_);
    SendMessageW(hwnd_, TTM_TRACKACTIVATE, FALSE, reinterpret_cast<LPARAM>(&ti));
    visible_ = false;
}

void Tooltip::ApplyDarkMode(bool dark)
{
    dark_ = dark;
    if (hwnd_) {
        ApplyDarkModeToWindow(hwnd_, dark);
    }
}
