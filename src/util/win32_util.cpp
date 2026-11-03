#include "win32_util.h"
#include <dwmapi.h>
#include <uxtheme.h>

void ApplyDarkModeToWindow(HWND hwnd, bool dark)
{
    const BOOL value = dark ? TRUE : FALSE;
    DwmSetWindowAttribute(hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE, &value, sizeof(value));

    // エクスプローラーのダークテーマを適用すると非クライアントスクロールバーも
    // ダーク化される（Windows 標準の挙動を借用）。
    SetWindowTheme(hwnd, dark ? L"DarkMode_Explorer" : L"Explorer", nullptr);
}

void SetCloaked(HWND hwnd, BOOL cloaked)
{
    DwmSetWindowAttribute(hwnd, DWMWA_CLOAK, &cloaked, sizeof(cloaked));
}

std::wstring QuoteCommandLineArg(std::wstring_view arg)
{
    size_t trailing = 0;
    while (trailing < arg.size() && arg[arg.size() - 1 - trailing] == L'\\') {
        ++trailing;
    }
    std::wstring quoted;
    quoted.reserve(arg.size() + trailing + 2);
    quoted += L'"';
    quoted += arg;
    quoted.append(trailing, L'\\');
    quoted += L'"';
    return quoted;
}

RECT CascadeRect(const RECT& rect, const RECT& work, int step) noexcept
{
    RECT moved = rect;
    OffsetRect(&moved, step, step);
    if (moved.right > work.right || moved.bottom > work.bottom) {
        OffsetRect(&moved, work.left - moved.left, work.top - moved.top);
    }
    return moved;
}

namespace {

constexpr DWORD kStartupPlacementFlags = STARTF_USEPOSITION | STARTF_USESIZE;

} // namespace

void LaunchSelfInNewWindow(HWND origin, std::wstring_view arg, int step)
{
    WINDOWPLACEMENT wp{ .length = sizeof(wp) };
    MONITORINFO mi{ .cbSize = sizeof(mi) };
    const std::wstring exe = ExePath();
    if (exe.empty() || !GetWindowPlacement(origin, &wp) || !GetMonitorInfoW(MonitorFromWindow(origin, MONITOR_DEFAULTTONEAREST), &mi)) {
        return;
    }
    // 最大化中も元の大きさを渡すため rcNormalPosition を使う。作業領域が基準なのでスクリーン座標に直す。
    RECT rect = wp.rcNormalPosition;
    OffsetRect(&rect, mi.rcWork.left - mi.rcMonitor.left, mi.rcWork.top - mi.rcMonitor.top);
    rect = CascadeRect(rect, mi.rcWork, step);

    std::wstring cmd = QuoteCommandLineArg(exe);
    if (!arg.empty()) {
        cmd += L' ';
        cmd += QuoteCommandLineArg(arg);
    }
    STARTUPINFOW si{
        .cb = sizeof(si),
        .dwX = static_cast<DWORD>(rect.left),
        .dwY = static_cast<DWORD>(rect.top),
        .dwXSize = static_cast<DWORD>(rect.right - rect.left),
        .dwYSize = static_cast<DWORD>(rect.bottom - rect.top),
        .dwFlags = kStartupPlacementFlags | STARTF_USESHOWWINDOW,
        .wShowWindow = static_cast<WORD>(IsZoomed(origin) ? SW_SHOWMAXIMIZED : SW_SHOWNORMAL),
    };
    PROCESS_INFORMATION pi{};
    if (CreateProcessW(exe.c_str(), cmd.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) {
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
    }
}

bool HasStartupPlacement()
{
    STARTUPINFOW si{ .cb = sizeof(si) };
    GetStartupInfoW(&si);
    return (si.dwFlags & kStartupPlacementFlags) == kStartupPlacementFlags;
}
