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
