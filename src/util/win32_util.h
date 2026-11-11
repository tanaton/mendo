#pragma once
#include "profiler.h"
#include <windows.h>
#include <cstddef>
#include <span>
#include <string>
#include <string_view>

// MAX_PATH に収まらなければ空。
template <typename GetPathFn>
std::wstring QueryPath(GetPathFn get_path)
{
    wchar_t buf[MAX_PATH];
    const UINT len = get_path(buf, MAX_PATH);
    return (len > 0 && len < MAX_PATH) ? std::wstring{ buf, len } : std::wstring{};
}

inline std::wstring ExePath()
{
    return QueryPath([](wchar_t* b, UINT n) static { return GetModuleFileNameW(nullptr, b, n); });
}

inline bool IsKeyDown(int vk) noexcept
{
    return (GetKeyState(vk) & 0x8000) != 0;
}

// OS の「一度にスクロールする行数」。WHEEL_PAGESCROLL なら 1 画面ずつ。読めなければ Windows の既定の 3 行。
inline UINT WheelScrollLines() noexcept
{
    UINT lines = 3;
    SystemParametersInfoW(SPI_GETWHEELSCROLLLINES, 0, &lines, 0);
    return lines;
}

// リソースはプロセスのアドレス空間にマップされておりコピー不要。
inline std::span<const std::byte> LoadRcData(UINT resource_id) noexcept
{
    const HRSRC hRes = FindResourceW(nullptr, MAKEINTRESOURCEW(resource_id), RT_RCDATA);
    if (!hRes) {
        return {};
    }
    const HGLOBAL hData = LoadResource(nullptr, hRes);
    if (!hData) {
        return {};
    }
    const DWORD size = SizeofResource(nullptr, hRes);
    const auto* data = static_cast<const std::byte*>(LockResource(hData));
    if (!data || size == 0) {
        return {};
    }
    return { data, size };
}

// WM_NCCREATE で lpCreateParams の T* を hwnd に結び付け、以降のメッセージではそれを返す。結び付け前は nullptr。
template <class T>
T* BindWindowInstance(HWND hwnd, UINT msg, LPARAM lParam) noexcept
{
    if (msg == WM_NCCREATE) {
        auto* self = static_cast<T*>(reinterpret_cast<const CREATESTRUCTW*>(lParam)->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        return self;
    }
    return reinterpret_cast<T*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
}

// hIconSm は渡さない。OS が icon のリソースから小さいサイズを選ぶ。
inline bool RegisterWindowClass(const wchar_t* name, WNDPROC proc, HICON icon = nullptr, HCURSOR cursor = nullptr, UINT style = 0) noexcept
{
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = style;
    wc.lpfnWndProc = proc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.hIcon = icon;
    wc.hCursor = cursor;
    wc.lpszClassName = name;
    return RegisterClassExW(&wc) != 0;
}

inline void LogHrFailure([[maybe_unused]] const char* what, HRESULT hr) noexcept
{
    if (FAILED(hr)) {
        MENDO_TRACEF("{} failed (hr=0x{:08X})", what, static_cast<unsigned long>(hr));
    }
}

// <dwmapi.h> / <uxtheme.h> をヘッダに漏らさないため実装は win32_util.cpp。
void ApplyDarkModeToWindow(HWND hwnd, bool dark);
void SetCloaked(HWND hwnd, BOOL cloaked);

// CRT と CommandLineToArgvW が 1 つの引数に戻す形で引用符に囲む。パスには " が無いが、リンクの見出し (--anchor) にはありうるので、
// " は \" にし、その前と末尾の \ は 2 倍にする。
std::wstring QuoteCommandLineArg(std::wstring_view arg);
// rect を step ずつ右下へずらす。work からはみ出すなら work の左上へ戻す。
// 左上が occupied (開いているウィンドウの左上) のどれかと重なる間は、さらにずらす。
RECT CascadeRect(const RECT& rect, const RECT& work, int step, std::span<const POINT> occupied) noexcept;
// この exe を origin から step ずらした位置の新しいウィンドウとして起動する。開いている同じクラスのウィンドウとは
// 左上が重ならないようにする。args は exe の名前の後ろに付ける引数。
// 位置と大きさは STARTUPINFO で渡すので、受け側は CreateWindowExW に CW_USEDEFAULT を渡せば使われる。
void LaunchSelfInNewWindow(HWND origin, std::span<const std::wstring_view> args, int step);
// LaunchSelfInNewWindow から起動され、位置と大きさを受け取った。
bool HasStartupPlacement();
