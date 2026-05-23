#pragma once
#include <windows.h>
#include <cstdio>

namespace mendo {

inline void LogHrFailure(const wchar_t* what, HRESULT hr) noexcept
{
    if (SUCCEEDED(hr)) {
        return;
    }
    wchar_t msg[160];
    swprintf_s(msg, L"[mendo] %ls failed (hr=0x%08lX)\n", what, static_cast<unsigned long>(hr));
    OutputDebugStringW(msg);
}

} // namespace mendo
