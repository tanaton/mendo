#include "tooltip.h"
#include <windows.h>
#include <commctrl.h>
#include <dwmapi.h>
#include <uxtheme.h>

#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "uxtheme.lib")

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

struct Tooltip::Impl {
    HWND hwnd = nullptr;
    HWND parent = nullptr;
    TooltipTarget current;
    bool visible = false;
    bool dark = false;

    bool EnsureWindow();

    ~Impl()
    {
        if (hwnd) {
            DestroyWindow(hwnd);
            hwnd = nullptr;
        }
    }
};

Tooltip::Tooltip() : impl_(std::make_unique<Impl>())
{}
Tooltip::~Tooltip() = default;
Tooltip::Tooltip(Tooltip&&) noexcept = default;
Tooltip& Tooltip::operator=(Tooltip&&) noexcept = default;

void Tooltip::Init(HWND parent_hwnd)
{
    impl_->parent = parent_hwnd;
}

// 起動時の初回描画を遅らせないよう、ウィンドウは最初の表示まで作らない。
bool Tooltip::Impl::EnsureWindow()
{
    if (hwnd || !parent) {
        return hwnd != nullptr;
    }
    hwnd = CreateWindowExW(
        WS_EX_TOPMOST,
        TOOLTIPS_CLASSW,
        nullptr,
        WS_POPUP | TTS_NOPREFIX | TTS_ALWAYSTIP,
        CW_USEDEFAULT, CW_USEDEFAULT,
        CW_USEDEFAULT, CW_USEDEFAULT,
        parent,
        nullptr,
        GetModuleHandleW(nullptr),
        nullptr);
    if (!hwnd) {
        return false;
    }

    // 位置と表示を手動で制御するため TTF_TRACK で登録する。
    TOOLINFOW ti = MakeToolInfo(parent);
    ti.uFlags = TTF_TRACK | TTF_ABSOLUTE;
    ti.lpszText = const_cast<LPWSTR>(L"");
    SendMessageW(hwnd, TTM_ADDTOOLW, 0, reinterpret_cast<LPARAM>(&ti));

    // 長い URL の折り返し用
    SendMessageW(hwnd, TTM_SETMAXTIPWIDTH, 0, 600);
    if (dark) {
        ApplyDarkModeToWindow(hwnd, true);
    }
    return true;
}

bool Tooltip::Update(const TooltipTarget& target)
{
    auto& s = *impl_;
    if (target.SameTarget(s.current)) {
        return false;
    }

    Hide();
    s.current = target;
    return !s.current.IsEmpty();
}

void Tooltip::Show()
{
    auto& s = *impl_;
    if (s.current.IsEmpty() || !s.EnsureWindow()) {
        return;
    }

    TOOLINFOW ti = MakeToolInfo(s.parent);
    ti.lpszText = const_cast<LPWSTR>(s.current.text.c_str());
    SendMessageW(s.hwnd, TTM_UPDATETIPTEXTW, 0, reinterpret_cast<LPARAM>(&ti));

    // 表示時点の実カーソル位置を使う。タイマー待機中に同一ターゲット内でカーソルが
    // 動いても最新位置で表示するため (同一ターゲットでは reducer 側で抑止され Update は
    // 呼ばれないので、保存した位置では陳腐化する)。
    POINT pt{};
    GetCursorPos(&pt);
    const UINT dpi = GetDpiForWindow(s.parent);
    const int offset_y = MulDiv(20, dpi, USER_DEFAULT_SCREEN_DPI);
    SendMessageW(s.hwnd, TTM_TRACKPOSITION, 0, MAKELPARAM(pt.x, pt.y + offset_y));

    SendMessageW(s.hwnd, TTM_TRACKACTIVATE, TRUE, reinterpret_cast<LPARAM>(&ti));
    s.visible = true;
}

void Tooltip::Hide()
{
    auto& s = *impl_;
    if (!s.hwnd || !s.visible) {
        return;
    }
    TOOLINFOW ti = MakeToolInfo(s.parent);
    SendMessageW(s.hwnd, TTM_TRACKACTIVATE, FALSE, reinterpret_cast<LPARAM>(&ti));
    s.visible = false;
}

void Tooltip::ApplyDarkMode(bool dark)
{
    impl_->dark = dark;
    if (impl_->hwnd) {
        ApplyDarkModeToWindow(impl_->hwnd, dark);
    }
}

void Tooltip::ResetTarget() noexcept
{
    impl_->current = {};
}

const TooltipTarget& Tooltip::GetCurrent() const noexcept
{
    return impl_->current;
}
