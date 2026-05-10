#pragma once
#include "win32_host.h"

class CursorManager;

// IWin32Host の本番 Win32 実装。HWND と CursorManager を保持し、
// 実際の Win32 API 呼び出しに委譲する。
class Win32Host final : public IWin32Host {
public:
    void Init(HWND hwnd, CursorManager& cursors) noexcept;

    void Invalidate() override;
    void InvalidateTitleBarArea(float dip_w, float dip_h, float dpi_scale) override;
    void SetTimer(app_timer::Id id, UINT ms) override;
    void KillTimer(app_timer::Id id) override;
    void SetCapture() override;
    void ReleaseCapture() override;
    void SetCursor(effect::CursorType type) override;
    void WriteClipboardText(std::string_view text) override;
    void WriteClipboardHtml(std::string_view html, std::string_view plain) override;
    void ShellOpen(const std::pmr::wstring& url) override;
    void ShowWindowCmd(int cmd) override;
    void PostWindowMessage(UINT msg, WPARAM wp, LPARAM lp) override;
    void SearchFocus(effect::SearchFocus action) override;
    void SearchUnfocus(effect::SearchUnfocus action) override;
    void SetWindowTitle(const std::pmr::wstring& title) override;
    void SetWindowPosition(int x, int y, int cx, int cy) override;
    POINT ClientToScreen(POINT client_pt) override;
    void ApplyDarkMode(bool dark) override;

private:
    HWND hwnd_ = nullptr;
    CursorManager* cursors_ = nullptr;
};
