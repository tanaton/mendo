#include "win32_host_impl.h"
#include "app_constants.h"
#include "clipboard_util.h"
#include "cursor_manager.h"
#include "d2d_util.h"
#include "darkmode_util.h"
#include "win_handle.h"
#include <memory_resource>
#include <shellapi.h>
#include <utility>

void Win32Host::Init(HWND hwnd, CursorManager& cursors) noexcept
{
    hwnd_ = hwnd;
    cursors_ = &cursors;
}

void Win32Host::Invalidate()
{
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void Win32Host::InvalidateTitleBarArea(float dip_w, float dip_h, float dpi_scale)
{
    if (dip_w <= 0.0f || dip_h <= 0.0f) {
        InvalidateRect(hwnd_, nullptr, FALSE);
        return;
    }
    mendo::InvalidateDipRect(hwnd_, 0.0f, 0.0f, dip_w, dip_h, dpi_scale);
}

void Win32Host::InvalidateMdPaneArea(float dip_x, float dip_y, float dip_w, float dip_h, float dpi_scale)
{
    // ゼロ矩形は no-op。executor 側で layout_cache 未確立時の全画面フォールバックを既に
    // 担っているため、ここで再度 InvalidateRect(nullptr) に倒すと最適化趣旨が打ち消される。
    if (dip_w <= 0.0f || dip_h <= 0.0f) {
        return;
    }
    mendo::InvalidateDipRect(hwnd_, dip_x, dip_y, dip_w, dip_h, dpi_scale);
}

void Win32Host::SetTimer(app_timer::Id id, UINT ms)
{
    ::SetTimer(hwnd_, std::to_underlying(id), ms, nullptr);
}

void Win32Host::KillTimer(app_timer::Id id)
{
    ::KillTimer(hwnd_, std::to_underlying(id));
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

void Win32Host::WriteClipboardText(std::string_view text)
{
    ::WriteClipboardText(hwnd_, text);
}

void Win32Host::WriteClipboardHtml(std::string_view html, std::string_view plain)
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
        PostMessageW(hwnd_, app_msg::SEARCH_FOCUS, app_param::SEARCH_FOCUS_SET_CARET, static_cast<LPARAM>(action.caret));
        break;
    case Mode::SetSelection: {
        // anchor / caret は LPARAM に pack 済みなので PostMessage 失敗・hwnd 破棄しても leak しない。
        const LPARAM lp = app_param::MakeSearchSelectionLParam(action.anchor, action.caret);
        PostMessageW(hwnd_, app_msg::SEARCH_FOCUS, app_param::SEARCH_FOCUS_SET_SELECTION, lp);
        break;
    }
    }
}

void Win32Host::SearchUnfocus(effect::SearchUnfocus action)
{
    const WPARAM wp = action.clear_text ? app_param::SEARCH_UNFOCUS_FILE_SWITCH : app_param::SEARCH_UNFOCUS_CLOSE;
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
