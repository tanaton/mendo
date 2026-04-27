#pragma once
#include "win32_host.h"

class CursorManager;

// IWin32Host の本番 Win32 実装。HWND と CursorManager を保持し、
// 実際の Win32 API 呼び出しに委譲する。
class Win32Host final : public IWin32Host {
public:
    void Init(HWND hwnd, CursorManager& cursors) noexcept;

    void Invalidate() override;
    void InvalidateTitleBarArea(int width_px, int height_px) override;
    void SetTimer(UINT_PTR id, UINT ms) override;
    void KillTimer(UINT_PTR id) override;
    void SetCapture() override;
    void ReleaseCapture() override;
    void SetCursor(effect::CursorType type) override;
    void WriteClipboardText(std::wstring_view text) override;
    void WriteClipboardHtml(std::wstring_view html, std::wstring_view plain) override;
    void ShellOpen(const std::pmr::wstring& url) override;
    void ShowWindowCmd(int cmd) override;
    void PostWindowMessage(UINT msg, WPARAM wp, LPARAM lp) override;
    void SetWindowTitle(const std::pmr::wstring& title) override;
    void SetWindowPosition(int x, int y, int cx, int cy) override;
    POINT ClientToScreen(POINT client_pt) override;
    void ApplyDarkMode(bool dark) override;

private:
    HWND hwnd_ = nullptr;
    CursorManager* cursors_ = nullptr;
};
