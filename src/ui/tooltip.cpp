#include "tooltip.h"
#include "darkmode_util.h"
#include <windows.h>
#include <commctrl.h>

namespace {
constexpr UINT_PTR TOOL_ID = 1;
}

struct Tooltip::Impl {
    HWND hwnd = nullptr;
    HWND parent = nullptr;
    TooltipTarget current;
    POINT show_pos{};
    bool visible = false;

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
    auto& s = *impl_;
    s.parent = parent_hwnd;
    s.hwnd = CreateWindowExW(
        WS_EX_TOPMOST,
        TOOLTIPS_CLASSW,
        nullptr,
        WS_POPUP | TTS_NOPREFIX | TTS_ALWAYSTIP,
        CW_USEDEFAULT, CW_USEDEFAULT,
        CW_USEDEFAULT, CW_USEDEFAULT,
        s.parent,
        nullptr,
        GetModuleHandleW(nullptr),
        nullptr);
    if (!s.hwnd) {
        return;
    }

    // TTF_TRACK ツールを1つだけ登録（手動で位置・表示を制御するため）
    TOOLINFOW ti{};
    ti.cbSize = sizeof(ti);
    ti.uFlags = TTF_TRACK | TTF_ABSOLUTE;
    ti.hwnd = s.parent;
    ti.uId = TOOL_ID;
    ti.lpszText = const_cast<LPWSTR>(L"");
    SendMessageW(s.hwnd, TTM_ADDTOOLW, 0, reinterpret_cast<LPARAM>(&ti));

    // 長い URL の折り返し用
    SendMessageW(s.hwnd, TTM_SETMAXTIPWIDTH, 0, 600);
}

bool Tooltip::Update(const TooltipTarget& target, int screen_x, int screen_y)
{
    auto& s = *impl_;
    if (target == s.current) {
        return false;
    }

    Hide();
    s.current = target;
    s.show_pos = POINT{ screen_x, screen_y };

    if (s.current.IsEmpty()) {
        return false;
    }
    return true;
}

void Tooltip::Show()
{
    auto& s = *impl_;
    if (!s.hwnd || s.current.IsEmpty()) {
        return;
    }

    TOOLINFOW ti{};
    ti.cbSize = sizeof(ti);
    ti.hwnd = s.parent;
    ti.uId = TOOL_ID;
    ti.lpszText = const_cast<LPWSTR>(s.current.text.c_str());
    SendMessageW(s.hwnd, TTM_UPDATETIPTEXTW, 0, reinterpret_cast<LPARAM>(&ti));

    const UINT dpi = GetDpiForWindow(s.parent);
    const int offset_y = MulDiv(20, dpi, 96);
    SendMessageW(s.hwnd, TTM_TRACKPOSITION, 0, MAKELPARAM(s.show_pos.x, s.show_pos.y + offset_y));

    SendMessageW(s.hwnd, TTM_TRACKACTIVATE, TRUE, reinterpret_cast<LPARAM>(&ti));
    s.visible = true;
}

void Tooltip::Hide()
{
    auto& s = *impl_;
    if (!s.hwnd || !s.visible) {
        return;
    }
    TOOLINFOW ti{};
    ti.cbSize = sizeof(ti);
    ti.hwnd = s.parent;
    ti.uId = TOOL_ID;
    SendMessageW(s.hwnd, TTM_TRACKACTIVATE, FALSE, reinterpret_cast<LPARAM>(&ti));
    s.visible = false;
}

void Tooltip::ApplyDarkMode(bool dark)
{
    auto& s = *impl_;
    if (s.hwnd) {
        ApplyDarkModeToWindow(s.hwnd, dark);
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
