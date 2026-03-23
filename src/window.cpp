#include "window.h"
#include <windowsx.h>
#include <shellscalingapi.h>

#pragma comment(lib, "shcore.lib")

static constexpr wchar_t WINDOW_CLASS[] = L"mendoWindow";

bool Win32Window::Create(HINSTANCE hInstance, int nCmdShow) {
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW | CS_DBLCLKS;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInstance;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = WINDOW_CLASS;
    wc.hbrBackground = nullptr;

    if (!RegisterClassExW(&wc)) return false;

    hwnd_ = CreateWindowExW(
        WS_EX_ACCEPTFILES,
        WINDOW_CLASS,
        L"mendo",
        WS_OVERLAPPEDWINDOW | WS_VSCROLL,
        CW_USEDEFAULT, CW_USEDEFAULT,
        1600, 900,
        nullptr, nullptr, hInstance, this);

    if (!hwnd_) return false;

    if (!app_.Init(hwnd_)) return false;

    ShowWindow(hwnd_, nCmdShow);
    UpdateWindow(hwnd_);

    return true;
}

int Win32Window::RunMessageLoop() {
    MSG msg{};
    BOOL ret;
    while ((ret = GetMessageW(&msg, nullptr, 0, 0)) != 0) {
        if (ret == -1) break;
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return static_cast<int>(msg.wParam);
}

LRESULT CALLBACK Win32Window::WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    Win32Window* self = nullptr;

    if (msg == WM_NCCREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
        self = static_cast<Win32Window*>(cs->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        self->hwnd_ = hwnd;
    } else {
        self = reinterpret_cast<Win32Window*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }

    if (self) {
        return self->HandleMessage(msg, wParam, lParam);
    }

    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

LRESULT Win32Window::HandleMessage(UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_PAINT:
            app_.OnPaint();
            return 0;

        case WM_SIZE:
            app_.OnResize(LOWORD(lParam), HIWORD(lParam));
            return 0;

        case WM_ENTERSIZEMOVE:
            app_.OnEnterSizeMove();
            return 0;

        case WM_EXITSIZEMOVE:
            app_.OnExitSizeMove();
            return 0;

        case WM_VSCROLL:
            app_.OnVScroll(wParam);
            return 0;

        case WM_RBUTTONDOWN:
            if (!app_.OnRButtonDown(GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)))
                return DefWindowProcW(hwnd_, msg, wParam, lParam);
            return 0;

        case WM_RBUTTONUP:
            if (!app_.OnRButtonUp(GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)))
                return DefWindowProcW(hwnd_, msg, wParam, lParam);
            return 0;

        case WM_LBUTTONDOWN:
            app_.OnLButtonDown(GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam));
            return 0;

        case WM_LBUTTONUP:
            app_.OnLButtonUp(GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam));
            return 0;

        case WM_MOUSEMOVE:
            if (wParam & MK_LBUTTON) {
                app_.OnMouseMove(GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam));
            } else if (wParam & MK_RBUTTON) {
                app_.OnRButtonMove(GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam));
            } else {
                app_.OnMouseHover(GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam));
            }
            return 0;

        case WM_SETCURSOR:
            if (LOWORD(lParam) == HTCLIENT) {
                return TRUE;
            }
            return DefWindowProcW(hwnd_, msg, wParam, lParam);

        case WM_LBUTTONDBLCLK:
            app_.OnLButtonDblClk(GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam));
            return 0;

        case WM_MOUSEWHEEL: {
            short wheel_delta = GET_WHEEL_DELTA_WPARAM(wParam);
            bool ctrl = (LOWORD(wParam) & MK_CONTROL) != 0;

            if (ctrl) {
                app_.OnMouseWheel(0, 0, wheel_delta, true);
            } else {
                POINT pt;
                pt.x = GET_X_LPARAM(lParam);
                pt.y = GET_Y_LPARAM(lParam);
                ScreenToClient(hwnd_, &pt);
                app_.OnMouseWheel(pt.x, pt.y, wheel_delta);
            }
            return 0;
        }

        case WM_CONTEXTMENU:
            app_.OnContextMenu(GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam));
            return 0;

        case WM_KEYDOWN:
            app_.OnKeyDown(wParam);
            return 0;

        case WM_XBUTTONDOWN: {
            WORD button = GET_XBUTTON_WPARAM(wParam);
            if (button == XBUTTON1) app_.OnXButtonBack();
            else if (button == XBUTTON2) app_.OnXButtonForward();
            return TRUE;
        }

        case WM_DROPFILES:
            app_.OnDropFiles(reinterpret_cast<HDROP>(wParam));
            return 0;

        case WM_DPICHANGED: {
            UINT dpi = HIWORD(wParam);
            auto* suggested = reinterpret_cast<const RECT*>(lParam);
            app_.OnDpiChanged(dpi, suggested);
            return 0;
        }

        case WM_TIMER:
            app_.HandleTimer(wParam);
            return 0;

        case App::WM_APP_LOAD_FILE:
            app_.OnAppLoadFile();
            return 0;

        case WM_CAPTURECHANGED:
            app_.OnCaptureChanged();
            return 0;

        case WM_DESTROY:
            app_.OnDestroy();
            PostQuitMessage(0);
            return 0;

        default:
            return DefWindowProcW(hwnd_, msg, wParam, lParam);
    }
}
