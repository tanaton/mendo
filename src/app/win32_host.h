#pragma once
#include "side_effect.h"
#include <memory_resource>
#include <string>
#include <windows.h>
#include <string_view>

// SideEffectExecutor が Win32 API 境界を跨ぐための adapter interface。
// 本番では Win32Host（HWND と CursorManager を保持）を、
// テストでは mock 実装を注入して副作用を検証する。
//
// PostMessage / SetWindowText は <windows.h> でマクロ化されるため、
// 衝突回避のため PostWindowMessage / SetWindowTitle と命名している。
class IWin32Host {
public:
    virtual ~IWin32Host() = default;

    virtual void Invalidate() = 0;
    virtual void InvalidateTitleBarArea(int width_px, int height_px) = 0;

    virtual void SetTimer(UINT_PTR id, UINT ms) = 0;
    virtual void KillTimer(UINT_PTR id) = 0;

    virtual void SetCapture() = 0;
    virtual void ReleaseCapture() = 0;

    virtual void SetCursor(effect::CursorType type) = 0;

    virtual void WriteClipboardText(std::wstring_view text) = 0;
    virtual void WriteClipboardHtml(std::wstring_view html, std::wstring_view plain) = 0;

    // ShellExecuteW / SetWindowTextW は null 終端文字列を要求するため、
    // 既に null 終端を保証している pmr::wstring を直接受ける。これにより
    // wstring_view 経由の null 終端コピーを 1 回省略できる。
    virtual void ShellOpen(const std::pmr::wstring& url) = 0;
    virtual void ShowWindowCmd(int cmd) = 0;
    virtual void PostWindowMessage(UINT msg, WPARAM wp, LPARAM lp) = 0;
    virtual void SetWindowTitle(const std::pmr::wstring& title) = 0;
    virtual void SetWindowPosition(int x, int y, int cx, int cy) = 0;
    virtual POINT ClientToScreen(POINT client_pt) = 0;

    virtual void ApplyDarkMode(bool dark) = 0;
};
