#pragma once
#include <windows.h>
#include <cstddef>
#include <cstdio>
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

inline void LogHrFailure(const wchar_t* what, HRESULT hr) noexcept
{
    if (SUCCEEDED(hr)) {
        return;
    }
    wchar_t msg[160];
    swprintf_s(msg, L"[mendo] %ls failed (hr=0x%08lX)\n", what, static_cast<unsigned long>(hr));
    OutputDebugStringW(msg);
}
