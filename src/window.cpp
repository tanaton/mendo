#include "window.h"
#include <windowsx.h>
#include <shellscalingapi.h>
#include <dwmapi.h>

#pragma comment(lib, "shcore.lib")
#pragma comment(lib, "dwmapi.lib")

static constexpr wchar_t WINDOW_CLASS[] = L"mendoWindow";

bool Win32Window::Create(HINSTANCE hInstance, int nCmdShow)
{
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW | CS_DBLCLKS;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInstance;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = WINDOW_CLASS;
    wc.hbrBackground = nullptr;

    if (!RegisterClassExW(&wc)) {
        return false;
    }

    hwnd_ = CreateWindowExW(
        WS_EX_ACCEPTFILES,
        WINDOW_CLASS,
        L"mendo",
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT,
        1600, 900,
        nullptr, nullptr, hInstance, this);

    if (!hwnd_) {
        return false;
    }

    if (!app_.Init(hwnd_)) {
        return false;
    }

    UpdateDwmFrame();

    ShowWindow(hwnd_, nCmdShow);
    UpdateWindow(hwnd_);

    return true;
}

void Win32Window::UpdateDwmFrame()
{
    // 1ピクセルだけ拡張してDWMのウィンドウシャドウ・アニメーションを有効化。
    // キャプションボタンは自前描画のため大きな拡張は不要。
    MARGINS margins = { 0, 0, 1, 0 };
    DwmExtendFrameIntoClientArea(hwnd_, &margins);
    SetWindowPos(hwnd_, nullptr, 0, 0, 0, 0,
        SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
}

int Win32Window::RunMessageLoop()
{
    MSG msg{};
    BOOL ret;
    while ((ret = GetMessageW(&msg, nullptr, 0, 0)) != 0) {
        if (ret == -1) break;
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return static_cast<int>(msg.wParam);
}

LRESULT CALLBACK Win32Window::WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    Win32Window* self = nullptr;

    if (msg == WM_NCCREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
        self = static_cast<Win32Window*>(cs->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        self->hwnd_ = hwnd;
    }
    else {
        self = reinterpret_cast<Win32Window*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }

    if (self) {
        return self->HandleMessage(msg, wParam, lParam);
    }

    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

LRESULT Win32Window::OnNcCalcSize(WPARAM wParam, LPARAM lParam)
{
    if (wParam == TRUE) {
        auto* params = reinterpret_cast<NCCALCSIZE_PARAMS*>(lParam);
        // NC領域を完全に除去: クライアント領域 = ウィンドウ全体。

        // 最大化時はフレーム厚分だけ内側に縮小（タスクバー隠れ防止）
        if (IsZoomed(hwnd_)) {
            UINT dpi = GetDpiForWindow(hwnd_);
            int frame_x = GetSystemMetricsForDpi(SM_CXFRAME, dpi)
                + GetSystemMetricsForDpi(SM_CXPADDEDBORDER, dpi);
            int frame_y = GetSystemMetricsForDpi(SM_CYFRAME, dpi)
                + GetSystemMetricsForDpi(SM_CXPADDEDBORDER, dpi);
            params->rgrc[0].top += frame_y;
            params->rgrc[0].left += frame_x;
            params->rgrc[0].right -= frame_x;
            params->rgrc[0].bottom -= frame_y;
        }
        return 0;
    }
    return DefWindowProcW(hwnd_, WM_NCCALCSIZE, wParam, lParam);
}

LRESULT Win32Window::OnNcHitTest(LPARAM lParam)
{
    POINT pt = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
    ScreenToClient(hwnd_, &pt);

    // 非最大化時のリサイズ枠判定（細めの枠でスクロールバーと干渉しないようにする）
    if (!IsZoomed(hwnd_)) {
        UINT dpi = GetDpiForWindow(hwnd_);
        // リサイズ枠は4DIPの細い領域（スクロールバーと重ならないようにする）
        int border = MulDiv(4, dpi, 96);
        // 上端はタイトルバーがないため標準のフレーム厚を使う
        int frame_y = GetSystemMetricsForDpi(SM_CYFRAME, dpi)
            + GetSystemMetricsForDpi(SM_CXPADDEDBORDER, dpi);

        RECT rc;
        GetClientRect(hwnd_, &rc);

        if (pt.y < frame_y) {
            if (pt.x < border) {
                return HTTOPLEFT;
            }
            if (pt.x >= rc.right - border) {
                return HTTOPRIGHT;
            }
            return HTTOP;
        }
        if (pt.y >= rc.bottom - border) {
            if (pt.x < border) {
                return HTBOTTOMLEFT;
            }
            if (pt.x >= rc.right - border) {
                return HTBOTTOMRIGHT;
            }
            return HTBOTTOM;
        }
        if (pt.x < border) {
            return HTLEFT;
        }
        if (pt.x >= rc.right - border) {
            float dpi_scale = app_.GetDpiScale();
            if (app_.IsOverMdScrollbar(pt.x / dpi_scale, pt.y / dpi_scale)) {
                return HTCLIENT;
            }
            return HTRIGHT;
        }
    }

    // タイトルバー領域のヒットテスト
    float dpi_scale = app_.GetDpiScale();
    float dip_x = pt.x / dpi_scale;
    float dip_y = pt.y / dpi_scale;
    float titlebar_height = app_.GetTitleBarHeightDip();

    if (dip_y < titlebar_height) {
        auto zone = app_.TitleBarHitTest(dip_x, dip_y);
        switch (zone) {
        case TitleBarHitZone::FileToggle:
        case TitleBarHitZone::TocToggle:
        case TitleBarHitZone::Minimize:
        case TitleBarHitZone::Maximize:
        case TitleBarHitZone::Close:
            return HTCLIENT;  // カスタムボタンはWM_LBUTTONDOWNで処理
        case TitleBarHitZone::Caption:
        default:
            return HTCAPTION;
        }
    }

    return HTCLIENT;
}

LRESULT Win32Window::HandleMessage(UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg) {
    case WM_NCCALCSIZE:
        return OnNcCalcSize(wParam, lParam);

    case WM_NCHITTEST:
        return OnNcHitTest(lParam);

    case WM_PAINT:
        app_.OnPaint();
        return 0;

    case WM_SIZE:
        app_.OnResize(LOWORD(lParam), HIWORD(lParam));
        return 0;

    case WM_ACTIVATE:
        app_.OnActivate(LOWORD(wParam) != WA_INACTIVE);
        return DefWindowProcW(hwnd_, msg, wParam, lParam);

    case WM_ENTERSIZEMOVE:
        app_.OnEnterSizeMove();
        return 0;

    case WM_EXITSIZEMOVE:
        app_.OnExitSizeMove();
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
        }
        else if (wParam & MK_RBUTTON) {
            app_.OnRButtonMove(GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam));
        }
        else {
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
        }
        else {
            POINT pt;
            pt.x = GET_X_LPARAM(lParam);
            pt.y = GET_Y_LPARAM(lParam);
            ScreenToClient(hwnd_, &pt);
            app_.OnMouseWheel(pt.x, pt.y, wheel_delta);
        }
        return 0;
    }

    case WM_MOUSEHWHEEL: {
        short wheel_delta = GET_WHEEL_DELTA_WPARAM(wParam);
        app_.OnMouseHWheel(wheel_delta);
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
        UpdateDwmFrame();
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
