#pragma once
#include "profiler.h"
#include <windows.h>
#include <cstddef>
#include <span>

inline bool IsKeyDown(int vk) noexcept
{
    return (GetKeyState(vk) & 0x8000) != 0;
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

inline void LogHrFailure([[maybe_unused]] const char* what, HRESULT hr) noexcept
{
    if (FAILED(hr)) {
        MENDO_TRACEF("{} failed (hr=0x{:08X})", what, static_cast<unsigned long>(hr));
    }
}
