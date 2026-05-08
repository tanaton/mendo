#include "win32_host_impl.h"
#include "app_constants.h"
#include "clipboard_util.h"
#include "cursor_manager.h"
#include "darkmode_util.h"
#include "win_handle.h"
#include <memory_resource>
#include <shellapi.h>

void Win32Host::Init(HWND hwnd, CursorManager& cursors) noexcept
{
    hwnd_ = hwnd;
    cursors_ = &cursors;
}

void Win32Host::Invalidate()
{
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void Win32Host::InvalidateTitleBarArea(int width_px, int height_px)
{
    if (width_px <= 0 || height_px <= 0) {
        InvalidateRect(hwnd_, nullptr, FALSE);
        return;
    }
    RECT tb_rect{ 0, 0, static_cast<LONG>(width_px), static_cast<LONG>(height_px) };
    InvalidateRect(hwnd_, &tb_rect, FALSE);
}

void Win32Host::SetTimer(UINT_PTR id, UINT ms)
{
    ::SetTimer(hwnd_, id, ms, nullptr);
}

void Win32Host::KillTimer(UINT_PTR id)
{
    ::KillTimer(hwnd_, id);
}

void Win32Host::SetCapture()
{
    ::SetCapture(hwnd_);
}

void Win32Host::ReleaseCapture()
{
    ::ReleaseCapture();
}

void Win32Host::SetCursor(effect::CursorType type)
{
    if (!cursors_) {
        return;
    }
    HCURSOR cursor = nullptr;
    switch (type) {
    case effect::CursorType::Arrow:
        cursor = cursors_->Arrow();
        break;
    case effect::CursorType::Hand:
        cursor = cursors_->Hand();
        break;
    case effect::CursorType::IBeam:
        cursor = cursors_->IBeam();
        break;
    case effect::CursorType::SizeWE:
        cursor = cursors_->SizeWE();
        break;
    }
    if (cursor) {
        ::SetCursor(cursor);
    }
}

void Win32Host::WriteClipboardText(std::wstring_view text)
{
    ::WriteClipboardText(hwnd_, text);
}

void Win32Host::WriteClipboardHtml(std::wstring_view html, std::wstring_view plain)
{
    ::WriteClipboardHtml(hwnd_, html, plain);
}

void Win32Host::ShellOpen(const std::pmr::wstring& url)
{
    ShellExecuteW(nullptr, L"open", url.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

void Win32Host::ShowWindowCmd(int cmd)
{
    ShowWindow(hwnd_, cmd);
}

void Win32Host::PostWindowMessage(UINT msg, WPARAM wp, LPARAM lp)
{
    PostMessageW(hwnd_, msg, wp, lp);
}

void Win32Host::SearchFocus(effect::SearchFocus action)
{
    using Mode = effect::SearchFocus::Mode;
    switch (action.mode) {
    case Mode::SelectAll:
        PostMessageW(hwnd_, app_msg::SEARCH_FOCUS, app_param::SEARCH_FOCUS_SELECT_ALL, 0);
        break;
    case Mode::SetCaret:
        PostMessageW(hwnd_, app_msg::SEARCH_FOCUS, app_param::SEARCH_FOCUS_SET_CARET,
                     static_cast<LPARAM>(action.caret));
        break;
    case Mode::SetSelection: {
        // 失敗時は所有権を回収して leak を防ぐ。
        const LPARAM lp = app_param::MakeSearchSelectionLParam(action.anchor, action.caret);
        if (!PostMessageW(hwnd_, app_msg::SEARCH_FOCUS, app_param::SEARCH_FOCUS_SET_SELECTION, lp)) {
            delete reinterpret_cast<app_param::SearchSelectionPayload*>(lp);
        }
        break;
    }
    }
}

void Win32Host::SearchUnfocus(effect::SearchUnfocus action)
{
    const WPARAM wp = action.clear_text
        ? app_param::SEARCH_UNFOCUS_FILE_SWITCH
        : app_param::SEARCH_UNFOCUS_CLOSE;
    PostMessageW(hwnd_, app_msg::SEARCH_UNFOCUS, wp, 0);
}

void Win32Host::SetWindowTitle(const std::pmr::wstring& title)
{
    SetWindowTextW(hwnd_, title.c_str());
}

void Win32Host::SetWindowPosition(int x, int y, int cx, int cy)
{
    SetWindowPos(hwnd_, nullptr, x, y, cx, cy, SWP_NOZORDER | SWP_NOACTIVATE);
}

POINT Win32Host::ClientToScreen(POINT client_pt)
{
    ::ClientToScreen(hwnd_, &client_pt);
    return client_pt;
}

void Win32Host::ApplyDarkMode(bool dark)
{
    ApplyDarkModeToWindow(hwnd_, dark);
}
