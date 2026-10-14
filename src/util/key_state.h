#pragma once
#include <windows.h>

inline bool IsKeyDown(int vk) noexcept
{
    return (GetKeyState(vk) & 0x8000) != 0;
}
