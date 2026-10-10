#include "win32_host_impl.h"
#include "app_constants.h"
#include "clipboard_util.h"
#include <shellapi.h>
#include <utility>

void Win32Host::Init(HWND hwnd) noexcept
{
    hwnd_ = hwnd;
}

void Win32Host::Invalidate()
{
    InvalidateRect(hwnd_, nullptr, FALSE);
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

void Win32Host::SetWindowPosition(int x, int y, int cx, int cy)
{
    SetWindowPos(hwnd_, nullptr, x, y, cx, cy, SWP_NOZORDER | SWP_NOACTIVATE);
}
