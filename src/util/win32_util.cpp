#include "win32_util.h"
#include <dwmapi.h>
#include <uxtheme.h>
#include <algorithm>
#include <iterator>
#include <thread>
#include <vector>

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

namespace {

constexpr DWORD kStartupPlacementFlags = STARTF_USEPOSITION | STARTF_USESIZE;

// 子はプロセスの初期化が終わるまでウィンドウを作らず、列挙に出てこない。素早く続けて開いても重ならないよう、
// 渡した位置をこの間は使用中とみなす。
constexpr ULONGLONG kPendingLaunchMs = 2000;

struct PendingLaunch {
    POINT origin;
    ULONGLONG tick;
};
// UI スレッドだけが触る。
std::vector<PendingLaunch> g_pending_launches;

// 最小化は -32000、最大化は枠の分だけ作業領域の外にあり、ずらした先と重ならないので除く。
std::vector<POINT> OccupiedOrigins(HWND origin, ULONGLONG now)
{
    std::erase_if(g_pending_launches, [now](const PendingLaunch& p) { return now - p.tick > kPendingLaunchMs; });
    std::vector<POINT> origins;
    for (const auto& p : g_pending_launches) {
        origins.push_back(p.origin);
    }
    wchar_t cls[256];
    if (GetClassNameW(origin, cls, static_cast<int>(std::size(cls))) == 0) {
        return origins;
    }
    for (HWND h = nullptr; (h = FindWindowExW(nullptr, h, cls, nullptr)) != nullptr;) {
        RECT r;
        if (!IsIconic(h) && !IsZoomed(h) && GetWindowRect(h, &r)) {
            origins.push_back({ r.left, r.top });
        }
    }
    return origins;
}

} // namespace

RECT CascadeRect(const RECT& rect, const RECT& work, int step, std::span<const POINT> occupied) noexcept
{
    RECT moved = rect;
    // 左上へ戻って一巡すると空きが無いこともあるので、回数を区切る。
    for (size_t i = 0; i <= occupied.size(); ++i) {
        OffsetRect(&moved, step, step);
        if (moved.right > work.right || moved.bottom > work.bottom) {
            OffsetRect(&moved, work.left - moved.left, work.top - moved.top);
        }
        if (std::ranges::none_of(occupied, [&moved](const POINT& p) { return p.x == moved.left && p.y == moved.top; })) {
            break;
        }
    }
    return moved;
}

void LaunchSelfInNewWindow(HWND origin, std::wstring_view arg, int step)
{
    MONITORINFO mi{ .cbSize = sizeof(mi) };
    std::wstring exe = ExePath();
    if (exe.empty() || !GetMonitorInfoW(MonitorFromWindow(origin, MONITOR_DEFAULTTONEAREST), &mi)) {
        return;
    }
    const bool maximized = IsZoomed(origin) != FALSE;
    RECT rect{};
    if (maximized) {
        // 元の大きさを渡す。rcNormalPosition は作業領域が基準なのでスクリーン座標に直す。
        // スナップ中はスナップ前の位置を返すので、最大化のときだけ使う。
        WINDOWPLACEMENT wp{ .length = sizeof(wp) };
        if (!GetWindowPlacement(origin, &wp)) {
            return;
        }
        rect = wp.rcNormalPosition;
        OffsetRect(&rect, mi.rcWork.left - mi.rcMonitor.left, mi.rcWork.top - mi.rcMonitor.top);
    }
    else if (!GetWindowRect(origin, &rect)) {
        return;
    }
    const ULONGLONG now = GetTickCount64();
    rect = CascadeRect(rect, mi.rcWork, step, OccupiedOrigins(origin, now));
    g_pending_launches.push_back({ { rect.left, rect.top }, now });

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
        .wShowWindow = static_cast<WORD>(maximized ? SW_SHOWMAXIMIZED : SW_SHOWNORMAL),
    };
    // ウイルス対策の検査などでプロセスの作成は数十 ms かかりうるので、UI スレッドを待たせない。
    std::thread([exe = std::move(exe), cmd = std::move(cmd), si]() mutable {
        PROCESS_INFORMATION pi{};
        if (CreateProcessW(exe.c_str(), cmd.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) {
            CloseHandle(pi.hThread);
            CloseHandle(pi.hProcess);
        }
    }).detach();
}

bool HasStartupPlacement()
{
    STARTUPINFOW si{ .cb = sizeof(si) };
    GetStartupInfoW(&si);
    return (si.dwFlags & kStartupPlacementFlags) == kStartupPlacementFlags;
}
