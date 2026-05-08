#pragma once
#include <windows.h>
#include <cstdio>

namespace mendo {

// HRESULT 失敗時のみ "[mendo] <what> failed (hr=0xXXXXXXXX)" を OutputDebugStringW へ流す。
// SUCCEEDED の場合は早期 return し追加コストなし。WebView2 / D2D / WIC 等の COM API 呼び
// 出しで「失敗時に最低限のログだけ欲しい」用途を共通化する。
inline void LogHrFailure(const wchar_t* what, HRESULT hr) noexcept
{
    if (SUCCEEDED(hr)) {
        return;
    }
    wchar_t msg[160];
    swprintf_s(msg, L"[mendo] %ls failed (hr=0x%08lX)\n",
               what, static_cast<unsigned long>(hr));
    OutputDebugStringW(msg);
}

} // namespace mendo
