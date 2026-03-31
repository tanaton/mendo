#include "window.h"
#include "config_store.h"
#include "resource.h"
#include <windowsx.h>
#include <shellscalingapi.h>
#include <dwmapi.h>
#include <climits>

#pragma comment(lib, "shcore.lib")
#pragma comment(lib, "dwmapi.lib")

static constexpr wchar_t WINDOW_CLASS[] = L"mendoWindow";

// システムメニュー（タスクバー右クリック）のカスタムコマンドID
// 0xF000以上はシステム予約（SC_KEYMENU=0xF100等）のため、下位4bitが0のカスタム値を使う
static constexpr UINT SC_RESET_WINDOW = 0x0010;

bool Win32Window::Create(HINSTANCE hInstance, int nCmdShow)
{
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW | CS_DBLCLKS;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInstance;
    wc.hIcon = LoadIconW(hInstance, MAKEINTRESOURCEW(IDI_APP_ICON));
    wc.hIconSm = LoadIconW(hInstance, MAKEINTRESOURCEW(IDI_APP_ICON));
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

    InitSystemMenu();
    UpdateDwmFrame();

    if (!RestoreWindowPlacement(nCmdShow)) {
        ShowWindow(hwnd_, nCmdShow);
    }
    UpdateWindow(hwnd_);

    return true;
}

void Win32Window::UpdateDwmFrame()
{
    // 1ピクセルだけ拡張してDWMのウィンドウシャドウ・アニメーションを有効化。
    // キャプションボタンは自前描画のため大きな拡張は不要。
    const MARGINS margins = { 0, 0, 1, 0 };
    DwmExtendFrameIntoClientArea(hwnd_, &margins);
    SetWindowPos(hwnd_, nullptr, 0, 0, 0, 0, SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
}

int Win32Window::RunMessageLoop()
{
    MSG msg{};
    BOOL ret;
    while ((ret = GetMessageW(&msg, nullptr, 0, 0)) != 0) {
        if (ret == -1) {
            break;
        }
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return static_cast<int>(msg.wParam);
}

LRESULT CALLBACK Win32Window::WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    Win32Window* self = nullptr;

    if (msg == WM_NCCREATE) {
        const auto* cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
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
            const UINT dpi = GetDpiForWindow(hwnd_);
            const int frame_x = GetSystemMetricsForDpi(SM_CXFRAME, dpi)
                + GetSystemMetricsForDpi(SM_CXPADDEDBORDER, dpi);
            const int frame_y = GetSystemMetricsForDpi(SM_CYFRAME, dpi)
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
        const UINT dpi = GetDpiForWindow(hwnd_);
        const int border = MulDiv(4, dpi, 96);
        // 上端はタイトルバーがないため標準のフレーム厚を使う
        const int frame_y = GetSystemMetricsForDpi(SM_CYFRAME, dpi)
            + GetSystemMetricsForDpi(SM_CXPADDEDBORDER, dpi);
        // 右辺はシステム標準幅で広めに確保（スクロールバーとの共存）
        const int right_border = GetSystemMetricsForDpi(SM_CXFRAME, dpi)
            + GetSystemMetricsForDpi(SM_CXPADDEDBORDER, dpi);

        RECT rc;
        GetClientRect(hwnd_, &rc);

        if (pt.y < frame_y) {
            if (pt.x < border) {
                return HTTOPLEFT;
            }
            if (pt.x >= rc.right - right_border) {
                return HTTOPRIGHT;
            }
            return HTTOP;
        }
        if (pt.y >= rc.bottom - border) {
            if (pt.x < border) {
                return HTBOTTOMLEFT;
            }
            if (pt.x >= rc.right - right_border) {
                return HTBOTTOMRIGHT;
            }
            return HTBOTTOM;
        }
        if (pt.x < border) {
            return HTLEFT;
        }
        if (pt.x >= rc.right - right_border) {
            // 内側部分のみスクロールバーを優先、最外側borderはリサイズを優先
            if (pt.x < rc.right - border) {
                const float dpi_scale = app_.GetDpiScale();
                if (app_.IsOverMdScrollbar(pt.x / dpi_scale, pt.y / dpi_scale)) {
                    return HTCLIENT;
                }
            }
            return HTRIGHT;
        }
    }

    // タイトルバー領域のヒットテスト
    const float dpi_scale = app_.GetDpiScale();
    const float dip_x = pt.x / dpi_scale;
    const float dip_y = pt.y / dpi_scale;
    const float titlebar_height = app_.GetTitleBarHeightDip();

    if (dip_y < titlebar_height) {
        const auto zone = app_.TitleBarHitTest(dip_x, dip_y);
        switch (zone) {
        case TitleBarHitZone::Icon:
            return HTSYSMENU;  // システムメニュー表示（ダブルクリックで閉じる）
        case TitleBarHitZone::Help:
        case TitleBarHitZone::ThemeToggle:
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

    case WM_NCLBUTTONDOWN:
        // システムメニュー表示時はフラグを立て、モーダルループ中の右クリック競合を防ぐ
        if (wParam == HTSYSMENU) {
            in_sys_menu_ = true;
            const auto r = DefWindowProcW(hwnd_, msg, wParam, lParam);
            in_sys_menu_ = false;
            return r;
        }
        return DefWindowProcW(hwnd_, msg, wParam, lParam);

    case WM_RBUTTONDOWN:
        if (in_sys_menu_) {
            return 0;
        }
        if (!app_.OnRButtonDown(GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam))) {
            return DefWindowProcW(hwnd_, msg, wParam, lParam);
        }
        return 0;

    case WM_RBUTTONUP:
        if (in_sys_menu_) {
            return 0;
        }
        if (!app_.OnRButtonUp(GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam))) {
            return DefWindowProcW(hwnd_, msg, wParam, lParam);
        }
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
        const short wheel_delta = GET_WHEEL_DELTA_WPARAM(wParam);
        const bool ctrl = (LOWORD(wParam) & MK_CONTROL) != 0;

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
        const short wheel_delta = GET_WHEEL_DELTA_WPARAM(wParam);
        app_.OnMouseHWheel(wheel_delta);
        return 0;
    }

    case WM_CONTEXTMENU: {
        // システムメニュー表示中はカスタムメニューを抑制
        if (in_sys_menu_) {
            return 0;
        }
        const int sx = GET_X_LPARAM(lParam);
        const int sy = GET_Y_LPARAM(lParam);
        // マウス由来の場合、タイトルバー上の右クリックは抑制する
        if (sx != -1 || sy != -1) {
            POINT pt = { sx, sy };
            ScreenToClient(hwnd_, &pt);
            const float dpi_scale = app_.GetDpiScale();
            const float dip_y = pt.y / dpi_scale;
            if (dip_y < app_.GetTitleBarHeightDip()) {
                return 0;
            }
        }
        app_.OnContextMenu(sx, sy);
        return 0;
    }

    case WM_KEYDOWN:
        app_.OnKeyDown(wParam);
        return 0;

    case WM_XBUTTONDOWN: {
        const WORD button = GET_XBUTTON_WPARAM(wParam);
        if (button == XBUTTON1) {
            app_.OnXButtonBack();
        }
        else if (button == XBUTTON2) {
            app_.OnXButtonForward();
        }
        return TRUE;
    }

    case WM_DROPFILES:
        app_.OnDropFiles(reinterpret_cast<HDROP>(wParam));
        return 0;

    case WM_DPICHANGED: {
        const UINT dpi = HIWORD(wParam);
        const auto* suggested = reinterpret_cast<const RECT*>(lParam);
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

    case App::WM_APP_RELOAD_FILE:
        app_.OnAppReloadFile();
        return 0;

    case App::WM_APP_IMAGE_LOADED:
        app_.OnAppImageLoaded();
        return 0;

    case WM_SYSCOMMAND:
        if ((wParam & 0xFFF0) == SC_RESET_WINDOW) {
            ResetWindowPlacement();
            return 0;
        }
        return DefWindowProcW(hwnd_, msg, wParam, lParam);

    case WM_CAPTURECHANGED:
        app_.OnCaptureChanged();
        return 0;

    case WM_DESTROY:
        SaveWindowPlacement();
        app_.OnDestroy();
        config::Save();
        PostQuitMessage(0);
        return 0;

    case WM_NCRBUTTONDOWN:
        // システムメニューアイコン上の右クリックを抑制
        // （システムメニュー表示中に右クリックメニューが重なるのを防ぐ）
        if (wParam == HTSYSMENU) {
            return 0;
        }
        return DefWindowProcW(hwnd_, msg, wParam, lParam);

    default:
        return DefWindowProcW(hwnd_, msg, wParam, lParam);
    }
}

// ============================================================
// システムメニュー（タスクバー右クリック）
// ============================================================

void Win32Window::InitSystemMenu()
{
    const HMENU menu = GetSystemMenu(hwnd_, FALSE);
    if (menu) {
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(menu, MF_STRING, SC_RESET_WINDOW, L"ウィンドウ位置をリセット(&R)");
    }
}

void Win32Window::ResetWindowPlacement()
{
    // 最大化・最小化を解除してから配置をリセットする
    if (IsZoomed(hwnd_) || IsIconic(hwnd_)) {
        ShowWindow(hwnd_, SW_RESTORE);
    }

    // プライマリモニターの作業領域に中央配置
    RECT work{};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
    const int work_w = work.right - work.left;
    const int work_h = work.bottom - work.top;

    constexpr int DEFAULT_W = 1600;
    constexpr int DEFAULT_H = 900;
    const int w = std::min(DEFAULT_W, work_w);
    const int h = std::min(DEFAULT_H, work_h);
    const int x = work.left + (work_w - w) / 2;
    const int y = work.top + (work_h - h) / 2;

    SetWindowPos(hwnd_, nullptr, x, y, w, h, SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
}

// ============================================================
// ウィンドウ配置の永続化
// ============================================================

void Win32Window::SaveWindowPlacement()
{
    WINDOWPLACEMENT wp{};
    wp.length = sizeof(wp);
    if (!GetWindowPlacement(hwnd_, &wp)) {
        return;
    }
    const auto& rc = wp.rcNormalPosition;
    config::SetInt("Window", "X", rc.left);
    config::SetInt("Window", "Y", rc.top);
    config::SetInt("Window", "Width", rc.right - rc.left);
    config::SetInt("Window", "Height", rc.bottom - rc.top);

    // 最小化中に閉じた場合も、元が最大化だったかを正しく保存する
    const bool was_maximized = (wp.showCmd == SW_SHOWMAXIMIZED) ||
        ((wp.showCmd == SW_SHOWMINIMIZED) && (wp.flags & WPF_RESTORETOMAXIMIZED));
    config::SetBool("Window", "Maximized", was_maximized);
}

bool Win32Window::RestoreWindowPlacement(int nCmdShow)
{
    // 保存済みのウィンドウサイズを読み込み（なければデフォルト表示へフォールバック）
    const int w = config::GetInt("Window", "Width", 0, 100, 100000);
    const int h = config::GetInt("Window", "Height", 0, 100, 100000);
    if (w == 0 || h == 0) {
        return false;
    }
    const int x = config::GetInt("Window", "X", 0, -100000, 100000);
    const int y = config::GetInt("Window", "Y", 0, -100000, 100000);
    const bool maximized = config::GetBool("Window", "Maximized", false);

    WINDOWPLACEMENT wp{};
    wp.length = sizeof(wp);
    wp.rcNormalPosition = { x, y, x + w, y + h };

    if (nCmdShow == SW_SHOWMINIMIZED || nCmdShow == SW_MINIMIZE) {
        wp.showCmd = SW_SHOWMINIMIZED;
        if (maximized) {
            wp.flags = WPF_RESTORETOMAXIMIZED;
        }
    }
    else {
        wp.showCmd = maximized ? SW_SHOWMAXIMIZED : SW_SHOWNORMAL;
    }

    SetWindowPlacement(hwnd_, &wp);
    return true;
}

void Win32Window::RestoreScrollPosition()
{
    const int node = config::GetInt("Session", "ScrollNode", -1, -1, 100000000);
    if (node < 0) {
        return;
    }
    const int offset = config::GetInt("Session", "ScrollOffset", 0, -100000, 100000);
    app_.SetPendingRestoreNode(node, offset);
}
