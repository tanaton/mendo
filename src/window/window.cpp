#include "window.h"
#include "app.h"
#include "app_constants.h"
#include "config_service.h"
#include "i18n.h"
#include "resource.h"
#include "ui_constants.h"
#include "profiler.h"
#include <windowsx.h>
#include <shellscalingapi.h>
#include <dwmapi.h>
#include <commctrl.h>
#include <imm.h>
#include <algorithm>

#pragma comment(lib, "shcore.lib")
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "imm32.lib")

namespace {

constexpr wchar_t WINDOW_CLASS[] = L"mendoWindow";

// システムメニュー（タスクバー右クリック）のカスタムコマンドID
// 0xF000以上はシステム予約（SC_KEYMENU=0xF100等）のため、下位4bitが0のカスタム値を使う
constexpr UINT SC_RESET_WINDOW = 0x0010;

// ini 永続化キー。既存設定との互換性のため値の変更は禁止。
using namespace std::literals;
constexpr auto kSectionWindow = "Window"sv;
constexpr auto kKeyWindowX = "X"sv;
constexpr auto kKeyWindowY = "Y"sv;
constexpr auto kKeyWindowWidth = "Width"sv;
constexpr auto kKeyWindowHeight = "Height"sv;
constexpr auto kKeyWindowMaximized = "Maximized"sv;

bool IsKeyDown(int vk) noexcept
{
    return (GetKeyState(vk) & 0x8000) != 0;
}

// IME 変換候補ウィンドウを入力フィールドの下、キャレット位置に合わせる。
void PlaceImeWindowsBelowCaret(HWND edit)
{
    const HIMC himc = ImmGetContext(edit);
    if (!himc) {
        return;
    }
    RECT rc{};
    GetClientRect(edit, &rc);

    DWORD sel_end = 0;
    SendMessageW(edit, EM_GETSEL, 0, reinterpret_cast<LPARAM>(&sel_end));
    LONG caret_x = 0;
    if (sel_end > 0) {
        const LRESULT pos = SendMessageW(edit, EM_POSFROMCHAR, sel_end - 1, 0);
        if (pos != -1) {
            caret_x = static_cast<SHORT>(LOWORD(pos)) + IME_CARET_X_OFFSET;
        }
    }

    const POINT ime_pos = { caret_x, rc.bottom - rc.top };
    COMPOSITIONFORM cf{};
    cf.dwStyle = CFS_POINT;
    cf.ptCurrentPos = ime_pos;
    ImmSetCompositionWindow(himc, &cf);
    CANDIDATEFORM cdf{};
    cdf.dwIndex = 0;
    cdf.dwStyle = CFS_CANDIDATEPOS;
    cdf.ptCurrentPos = ime_pos;
    ImmSetCandidateWindow(himc, &cdf);
    ImmReleaseContext(edit, himc);
}

// 確定前のコンポジション文字列 (D2D 描画用)。取得できなければ空。
std::pmr::wstring GetImeCompositionString(HIMC himc)
{
    const LONG bytes = ImmGetCompositionStringW(himc, GCS_COMPSTR, nullptr, 0);
    if (bytes <= 0) {
        return {};
    }
    std::pmr::wstring comp(static_cast<size_t>(bytes) / sizeof(wchar_t), L'\0');
    ImmGetCompositionStringW(himc, GCS_COMPSTR, comp.data(), static_cast<DWORD>(bytes));
    return comp;
}

} // namespace

Win32Window::Win32Window(ConfigService& config)
    : config_(config), app_(std::make_unique<App>(config))
{
}
Win32Window::~Win32Window() = default;

void Win32Window::LoadHelpDocument()
{
    app_->LoadHelpDocument();
}
void Win32Window::SetInitialDirectory(std::wstring_view dir_path)
{
    app_->SetInitialDirectory(dir_path);
}
void Win32Window::StartPreloadAsync(std::pmr::wstring path)
{
    app_->StartPreloadAsync(std::move(path));
}

bool Win32Window::Create(HINSTANCE hInstance, int nCmdShow)
{
    MENDO_PROFILE("Win32Window::Create");
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
        DEFAULT_WINDOW_WIDTH, DEFAULT_WINDOW_HEIGHT,
        nullptr, nullptr, hInstance, this);

    if (!hwnd_) {
        return false;
    }

    if (!app_->Init(hwnd_)) {
        DestroyWindow(hwnd_);
        hwnd_ = nullptr;
        return false;
    }

    // IME 対応のため、検索入力は非表示の EDIT コントロールで受ける
    search_edit_ = CreateWindowExW(0, L"EDIT", L"", WS_CHILD | ES_AUTOHSCROLL, 0, 0, 1, 1, hwnd_, nullptr, hInstance, nullptr);
    if (search_edit_) {
        SetWindowSubclass(search_edit_, SearchEditProc, 0, reinterpret_cast<DWORD_PTR>(this));
    }

    InitSystemMenu();
    UpdateDwmFrame();
    UpdateDpiMetricsCache();

    if (!RestoreWindowPlacement(nCmdShow)) {
        ShowWindow(hwnd_, nCmdShow);
    }
    UpdateWindow(hwnd_);

    return true;
}

void Win32Window::UpdateDpiMetricsCache()
{
    MENDO_PROFILE("Win32Window::UpdateDpiMetricsCache");
    if (!hwnd_) {
        return;
    }
    const UINT dpi = GetDpiForWindow(hwnd_);
    cached_nchit_border_ = MulDiv(4, dpi, 96);
    cached_nchit_frame_y_ = GetSystemMetricsForDpi(SM_CYFRAME, dpi) + GetSystemMetricsForDpi(SM_CXPADDEDBORDER, dpi);
    cached_nchit_right_border_ = GetSystemMetricsForDpi(SM_CXFRAME, dpi) + GetSystemMetricsForDpi(SM_CXPADDEDBORDER, dpi);
}

void Win32Window::UpdateDwmFrame()
{
    MENDO_PROFILE("Win32Window::UpdateDwmFrame");
    // 1ピクセルだけ拡張してDWMのウィンドウシャドウ・アニメーションを有効化。
    // キャプションボタンは自前描画のため大きな拡張は不要。
    const MARGINS margins = { 0, 0, 1, 0 };
    DwmExtendFrameIntoClientArea(hwnd_, &margins);
    SetWindowPos(hwnd_, nullptr, 0, 0, 0, 0, SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
}

int Win32Window::RunMessageLoop()
{
    MSG msg{};

    for (;;) {
        // イベントハンドルは DispatchMessageW 中に変わりうるため毎回取得
        const HANDLE evt = app_->GetFileWatchEvent();
        const DWORD count = evt ? 1 : 0;
        const DWORD wait = MsgWaitForMultipleObjects(count, evt ? &evt : nullptr, FALSE, INFINITE, QS_ALLINPUT);

        if (wait == WAIT_FAILED) {
            // ハンドル無効時のビジーループ防止
            MsgWaitForMultipleObjects(0, nullptr, FALSE, INFINITE, QS_ALLINPUT);
        }
        else if (wait < WAIT_OBJECT_0 + count) {
            app_->OnFileWatchEvent();
        }

        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) {
                return static_cast<int>(msg.wParam);
            }
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
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
    if (wParam != TRUE) {
        return DefWindowProcW(hwnd_, WM_NCCALCSIZE, wParam, lParam);
    }
    // NC領域を完全に除去: クライアント領域 = ウィンドウ全体。
    // 最大化時はフレーム厚分だけ内側に縮小（タスクバー隠れ防止）
    if (IsZoomed(hwnd_)) {
        auto& rc = reinterpret_cast<NCCALCSIZE_PARAMS*>(lParam)->rgrc[0];
        rc.top += cached_nchit_frame_y_;
        rc.left += cached_nchit_right_border_;
        rc.right -= cached_nchit_right_border_;
        rc.bottom -= cached_nchit_frame_y_;
    }
    return 0;
}

LRESULT Win32Window::OnNcHitTest(LPARAM lParam)
{
    POINT pt = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
    ScreenToClient(hwnd_, &pt);

    if (const LRESULT frame_hit = HitTestResizeFrame(pt); frame_hit != HTNOWHERE) {
        return frame_hit;
    }
    return HitTestTitleBar(pt);
}

LRESULT Win32Window::HitTestResizeFrame(POINT pt) const noexcept
{
    if (IsZoomed(hwnd_)) {
        return HTNOWHERE;
    }

    const int border = cached_nchit_border_;
    const int frame_y = cached_nchit_frame_y_;
    const int right_border = cached_nchit_right_border_;

    RECT rc{};
    if (!GetClientRect(hwnd_, &rc)) {
        return HTNOWHERE;
    }

    const bool left = pt.x < border;
    const bool right = pt.x >= rc.right - right_border;
    if (pt.y < frame_y) {
        return left ? HTTOPLEFT : (right ? HTTOPRIGHT : HTTOP);
    }
    if (pt.y >= rc.bottom - border) {
        return left ? HTBOTTOMLEFT : (right ? HTBOTTOMRIGHT : HTBOTTOM);
    }
    if (left) {
        return HTLEFT;
    }
    if (!right) {
        return HTNOWHERE;
    }
    // 右辺: スクロールバー領域は HTCLIENT を優先するが、最外側 border はリサイズを優先
    if (pt.x < rc.right - border) {
        const auto dip = PixelToDip(pt.x, pt.y, app_->GetDpiScale());
        if (app_->IsOverMdScrollbar(dip.x, dip.y)) {
            return HTCLIENT;
        }
    }
    return HTRIGHT;
}

LRESULT Win32Window::HitTestTitleBar(POINT pt) const noexcept
{
    const auto [dip_x, dip_y] = PixelToDip(pt.x, pt.y, app_->GetDpiScale());
    if (dip_y >= app_->GetTitleBarHeightDip()) {
        return HTCLIENT;
    }

    switch (app_->TitleBarHitTest(dip_x, dip_y)) {
    case TitleBarHitZone::Icon:
        return HTSYSMENU; // システムメニュー表示（ダブルクリックで閉じる）
    case TitleBarHitZone::OpenFile:
    case TitleBarHitZone::Help:
    case TitleBarHitZone::ThemeToggle:
    case TitleBarHitZone::Search:
    case TitleBarHitZone::FileToggle:
    case TitleBarHitZone::TocToggle:
    case TitleBarHitZone::Minimize:
    case TitleBarHitZone::Maximize:
    case TitleBarHitZone::Close:
        return HTCLIENT; // カスタムボタンはWM_LBUTTONDOWNで処理
    case TitleBarHitZone::Caption:
    default:
        return HTCAPTION;
    }
}

LRESULT Win32Window::HandleMouseMessage(UINT msg, WPARAM wParam, LPARAM lParam)
{
    // WM_MOUSEWHEEL / WM_CONTEXTMENU ではスクリーン座標、それ以外はクライアント座標。
    const int x = GET_X_LPARAM(lParam);
    const int y = GET_Y_LPARAM(lParam);

    switch (msg) {
    case WM_RBUTTONDOWN:
        if (in_sys_menu_) {
            return 0;
        }
        if (!app_->OnRButtonDown(x, y)) {
            return DefWindowProcW(hwnd_, msg, wParam, lParam);
        }
        return 0;

    case WM_RBUTTONUP:
        if (in_sys_menu_) {
            return 0;
        }
        if (!app_->OnRButtonUp(x, y)) {
            return DefWindowProcW(hwnd_, msg, wParam, lParam);
        }
        return 0;

    case WM_LBUTTONDOWN:
        app_->OnLButtonDown(x, y);
        return 0;

    case WM_LBUTTONUP:
        app_->OnLButtonUp(x, y);
        return 0;

    case WM_MOUSEMOVE:
        if (!tracking_mouse_) {
            TRACKMOUSEEVENT tme{};
            tme.cbSize = sizeof(tme);
            tme.dwFlags = TME_LEAVE;
            tme.hwndTrack = hwnd_;
            TrackMouseEvent(&tme);
            tracking_mouse_ = true;
        }
        if (wParam & MK_LBUTTON) {
            app_->OnMouseMove(x, y);
        }
        else if (wParam & MK_RBUTTON) {
            app_->OnRButtonMove(x, y);
        }
        else {
            app_->OnMouseHover(x, y);
        }
        return 0;

    case WM_MOUSELEAVE:
        tracking_mouse_ = false;
        app_->OnMouseLeave();
        return 0;

    case WM_SETCURSOR:
        if (LOWORD(lParam) == HTCLIENT) {
            return TRUE;
        }
        return DefWindowProcW(hwnd_, msg, wParam, lParam);

    case WM_LBUTTONDBLCLK:
        app_->OnLButtonDblClk(x, y);
        return 0;

    case WM_MOUSEWHEEL: {
        const short wheel_delta = GET_WHEEL_DELTA_WPARAM(wParam);
        const auto keys = LOWORD(wParam);
        if (keys & MK_CONTROL) {
            app_->OnMouseWheel(0, 0, wheel_delta, true);
        }
        else if (keys & MK_SHIFT) {
            // Shift+Wheel は横スクロール扱い。Web ブラウザの慣習に合わせ wheel-down を右スクロールに反転する。
            app_->OnMouseHWheel(static_cast<short>(-wheel_delta));
        }
        else {
            POINT pt = { x, y };
            ScreenToClient(hwnd_, &pt);
            app_->OnMouseWheel(pt.x, pt.y, wheel_delta);
        }
        return 0;
    }

    case WM_MOUSEHWHEEL:
        app_->OnMouseHWheel(GET_WHEEL_DELTA_WPARAM(wParam));
        return 0;

    case WM_CONTEXTMENU:
        OnContextMenu(x, y);
        return 0;

    case WM_XBUTTONDOWN: {
        const WORD button = GET_XBUTTON_WPARAM(wParam);
        if (button == XBUTTON1) {
            app_->OnXButtonBack();
        }
        else if (button == XBUTTON2) {
            app_->OnXButtonForward();
        }
        return TRUE;
    }

    default:
        break;
    }
    return DefWindowProcW(hwnd_, msg, wParam, lParam);
}

void Win32Window::OnContextMenu(int screen_x, int screen_y)
{
    // システムメニュー表示中はカスタムメニューを抑制
    if (in_sys_menu_) {
        return;
    }
    // キーボード起動 (VK_APPS / Shift+F10) は lParam が座標を持たない
    if (screen_x == -1 && screen_y == -1) {
        const POINT pt = KeyboardContextMenuPoint();
        app_->OnContextMenu(pt.x, pt.y);
        return;
    }
    // マウス由来の場合、タイトルバー上の右クリックは抑制する
    POINT pt = { screen_x, screen_y };
    ScreenToClient(hwnd_, &pt);
    if (PixelToDip(static_cast<float>(pt.y), app_->GetDpiScale()) < app_->GetTitleBarHeightDip()) {
        return;
    }
    app_->OnContextMenu(screen_x, screen_y);
}

POINT Win32Window::KeyboardContextMenuPoint() const
{
    // カーソルがクライアント内にあればその位置、なければクライアント中央 (スクリーン座標)。
    RECT rc{};
    GetClientRect(hwnd_, &rc);
    POINT pt{};
    if (GetCursorPos(&pt)) {
        POINT client = pt;
        ScreenToClient(hwnd_, &client);
        if (PtInRect(&rc, client)) {
            return pt;
        }
    }
    pt = { (rc.left + rc.right) / 2, (rc.top + rc.bottom) / 2 };
    ClientToScreen(hwnd_, &pt);
    return pt;
}

LRESULT Win32Window::HandleAppNotification(UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg) {
    case app_msg::IMAGE_LOADED:
        app_->OnAppImageLoaded();
        return 0;

    case app_msg::PARSE_COMPLETE:
        app_->OnParseComplete();
        return 0;

    case app_msg::MERMAID_DISK_LOADED:
        app_->OnMermaidDiskLoaded();
        return 0;

    case app_msg::SEARCH_FOCUS:
        FocusSearchEdit(wParam, lParam);
        return 0;

    case app_msg::SEARCH_UNFOCUS:
        SetFocus(hwnd_);
        if (wParam == app_param::SEARCH_UNFOCUS_FILE_SWITCH && search_edit_) {
            SetWindowTextW(search_edit_, L"");
        }
        return 0;

    default:
        break;
    }
    return DefWindowProcW(hwnd_, msg, wParam, lParam);
}

LRESULT Win32Window::HandleMessage(UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg) {
    case WM_RBUTTONDOWN:
    case WM_RBUTTONUP:
    case WM_LBUTTONDOWN:
    case WM_LBUTTONUP:
    case WM_MOUSEMOVE:
    case WM_MOUSELEAVE:
    case WM_SETCURSOR:
    case WM_LBUTTONDBLCLK:
    case WM_MOUSEWHEEL:
    case WM_MOUSEHWHEEL:
    case WM_CONTEXTMENU:
    case WM_XBUTTONDOWN:
        return HandleMouseMessage(msg, wParam, lParam);
    default:
        break;
    }

    if (msg >= WM_APP && msg < app_msg::END) {
        return HandleAppNotification(msg, wParam, lParam);
    }

    switch (msg) {
    case WM_NCCALCSIZE:
        return OnNcCalcSize(wParam, lParam);

    case WM_NCHITTEST:
        return OnNcHitTest(lParam);

    case WM_PAINT:
        app_->OnPaint();
        return 0;

    case WM_SIZE:
        OnSize(wParam, lParam);
        return 0;

    case WM_ACTIVATE:
        app_->OnActivate(LOWORD(wParam) != WA_INACTIVE);
        return DefWindowProcW(hwnd_, msg, wParam, lParam);

    case WM_ENTERSIZEMOVE:
        app_->OnEnterSizeMove();
        return 0;

    case WM_EXITSIZEMOVE:
        app_->OnExitSizeMove();
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

    case WM_KEYDOWN:
        app_->OnKeyDown(wParam);
        return 0;

    case WM_COMMAND:
        if (HIWORD(wParam) == EN_CHANGE && reinterpret_cast<HWND>(lParam) == search_edit_) {
            OnSearchEditChanged();
            return 0;
        }
        break;

    case WM_DROPFILES:
        app_->OnDropFiles(reinterpret_cast<HDROP>(wParam));
        return 0;

    case WM_DPICHANGED:
        app_->OnDpiChanged(HIWORD(wParam), reinterpret_cast<const RECT*>(lParam));
        UpdateDwmFrame();
        UpdateDpiMetricsCache();
        return 0;

    case WM_TIMER:
        app_->HandleTimer(wParam);
        return 0;

    case WM_SYSCOMMAND:
        if ((wParam & 0xFFF0) == SC_RESET_WINDOW) {
            ResetWindowPlacement();
            return 0;
        }
        return DefWindowProcW(hwnd_, msg, wParam, lParam);

    case WM_CAPTURECHANGED:
        // 自分自身への SetCapture (ドラッグ開始) でも届くため、喪失時のみ処理する
        if (reinterpret_cast<HWND>(lParam) != hwnd_) {
            app_->OnCaptureChanged();
        }
        return 0;

    case WM_DESTROY:
        SaveWindowPlacement();
        // dwRefData=this を渡しているので、Win32Window 破棄前に subclass を外さないと
        // 破棄後の SearchEditProc 呼び出しで dangling pointer を踏む。
        if (search_edit_) {
            RemoveWindowSubclass(search_edit_, SearchEditProc, 0);
        }
        app_->OnDestroy();
        PostQuitMessage(0);
        return 0;

    case WM_NCRBUTTONDOWN:
        // システムメニュー表示中に右クリックメニューが重なるのを防ぐ
        if (wParam == HTSYSMENU) {
            return 0;
        }
        return DefWindowProcW(hwnd_, msg, wParam, lParam);

    default:
        break;
    }
    return DefWindowProcW(hwnd_, msg, wParam, lParam);
}

void Win32Window::OnSize(WPARAM wParam, LPARAM lParam)
{
    // Windows 11 は SIZE_MINIMIZED 時に (0,0) ではなくタスクバーサムネイル寸法
    // （例: 237×39）を通知してくる。そのままResizeBuffersを呼ぶとスワップチェーンが
    // その小サイズになり、DWM合成時にアプリサイズへ引き伸ばされてフラッシュになる。
    if (wParam == SIZE_MINIMIZED) {
        was_minimized_ = true;
        return;
    }
    app_->OnResize(LOWORD(lParam), HIWORD(lParam));
    RepositionSearchEdit();
    // ResizeBuffers直後のバックバッファは未定義。DWMが復元アニメーション中に
    // その未定義フレームを合成してしまう前に、同期的にWM_PAINTを走らせて
    // 新フレームをPresentしておく。対話的リサイズでは不要なので、
    // 最小化からの復元時に限定する。
    if (was_minimized_ && (wParam == SIZE_RESTORED || wParam == SIZE_MAXIMIZED)) {
        UpdateWindow(hwnd_);
        was_minimized_ = false;
    }
}

// システムメニュー（タスクバー右クリック）
void Win32Window::InitSystemMenu()
{
    MENDO_PROFILE("Win32Window::InitSystemMenu");
    const HMENU menu = GetSystemMenu(hwnd_, FALSE);
    if (menu) {
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(menu, MF_STRING, SC_RESET_WINDOW, i18n::S().menu_reset_window.data());
    }
}

void Win32Window::ResetWindowPlacement()
{
    if (IsZoomed(hwnd_) || IsIconic(hwnd_)) {
        ShowWindow(hwnd_, SW_RESTORE);
    }

    // プライマリモニターの作業領域に中央配置
    RECT work{};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
    const int work_w = work.right - work.left;
    const int work_h = work.bottom - work.top;

    const int w = std::min(DEFAULT_WINDOW_WIDTH, work_w);
    const int h = std::min(DEFAULT_WINDOW_HEIGHT, work_h);
    const int x = work.left + (work_w - w) / 2;
    const int y = work.top + (work_h - h) / 2;

    SetWindowPos(hwnd_, nullptr, x, y, w, h, SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
}

void Win32Window::SaveWindowPlacement()
{
    WINDOWPLACEMENT wp{};
    wp.length = sizeof(wp);
    if (!GetWindowPlacement(hwnd_, &wp)) {
        return;
    }
    const auto& rc = wp.rcNormalPosition;
    config_.SaveInt(kSectionWindow, kKeyWindowX, rc.left);
    config_.SaveInt(kSectionWindow, kKeyWindowY, rc.top);
    config_.SaveInt(kSectionWindow, kKeyWindowWidth, rc.right - rc.left);
    config_.SaveInt(kSectionWindow, kKeyWindowHeight, rc.bottom - rc.top);

    // 最小化中に閉じた場合も、元が最大化だったかを正しく保存する
    const bool was_maximized = (wp.showCmd == SW_SHOWMAXIMIZED) || ((wp.showCmd == SW_SHOWMINIMIZED) && (wp.flags & WPF_RESTORETOMAXIMIZED));
    config_.SaveBool(kSectionWindow, kKeyWindowMaximized, was_maximized);
}

bool Win32Window::RestoreWindowPlacement(int nCmdShow)
{
    // 未保存 (0) ならデフォルト表示へフォールバックさせる
    const int w = config_.LoadInt(kSectionWindow, kKeyWindowWidth, 0, 100, 100000);
    const int h = config_.LoadInt(kSectionWindow, kKeyWindowHeight, 0, 100, 100000);
    if (w == 0 || h == 0) {
        return false;
    }
    const int x = config_.LoadInt(kSectionWindow, kKeyWindowX, 0, -100000, 100000);
    const int y = config_.LoadInt(kSectionWindow, kKeyWindowY, 0, -100000, 100000);
    const bool maximized = config_.LoadBool(kSectionWindow, kKeyWindowMaximized, false);

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
    const SessionService session{ config_ };
    const auto pos = session.LoadScrollPosition();
    if (pos.node < 0) {
        return;
    }
    app_->SetPendingRestoreNode(pos.node, pos.offset);
}

LRESULT CALLBACK Win32Window::SearchEditProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam, UINT_PTR /*uIdSubclass*/, DWORD_PTR dwRefData)
{
    auto* self = reinterpret_cast<Win32Window*>(dwRefData);

    switch (msg) {
    case WM_CHAR:
        // 単行EDITは\rを受け取るとビープ音を鳴らすので抑制
        if (wParam == L'\r') {
            return 0;
        }
        break;

    case WM_KEYDOWN:
        return self->OnSearchEditKeyDown(hwnd, wParam, lParam);

    case WM_IME_STARTCOMPOSITION:
        PlaceImeWindowsBelowCaret(hwnd);
        // DefSubclassProcに渡すと非表示EDITのデフォルト処理で位置が上書きされるため、ここでreturn
        return 0;

    case WM_IME_COMPOSITION:
        if (lParam & GCS_COMPSTR) {
            if (const HIMC himc = ImmGetContext(hwnd)) {
                self->app_->SetImeComposition(GetImeCompositionString(himc));
                ImmReleaseContext(hwnd, himc);
            }
        }
        break;

    case WM_IME_ENDCOMPOSITION:
        self->app_->SetImeComposition(L"");
        break;

    default:
        break;
    }
    return DefSubclassProc(hwnd, msg, wParam, lParam);
}

LRESULT Win32Window::OnSearchEditKeyDown(HWND edit, WPARAM wParam, LPARAM lParam)
{
    const bool ctrl = IsKeyDown(VK_CONTROL);
    const auto close = [this] {
        app_->OnSearchClose();
        SetFocus(hwnd_);
        return 0;
    };
    const auto step = [this] {
        if (IsKeyDown(VK_SHIFT)) {
            app_->OnSearchPrev();
        }
        else {
            app_->OnSearchNext();
        }
        return 0;
    };

    switch (wParam) {
    case VK_ESCAPE:
        return close();
    case VK_RETURN:
    case VK_F3:
        return step();
    case 'F':
        if (ctrl) {
            return close();
        }
        break;
    case 'G':
        if (ctrl) {
            return step();
        }
        break;
    case 'A':
        if (ctrl) {
            // EDIT 内の全選択に留め、メインウィンドウに伝播させない
            SendMessageW(edit, EM_SETSEL, 0, -1);
            SyncSearchCaretFromEdit();
            return 0;
        }
        break;
    default:
        break;
    }

    // 方向キー等: DefSubclassProcに処理させた後、キャレット位置を同期
    const LRESULT result = DefSubclassProc(edit, WM_KEYDOWN, wParam, lParam);
    SyncSearchCaretFromEdit();
    return result;
}

void Win32Window::OnSearchEditChanged()
{
    const int text_len = GetWindowTextLengthW(search_edit_);
    search_text_buf_.assign(static_cast<size_t>(std::max(text_len, 0)), L'\0');
    const int copied = GetWindowTextW(search_edit_, search_text_buf_.data(), text_len + 1);
    search_text_buf_.resize(static_cast<size_t>(std::max(copied, 0)));
    app_->OnSearchTextChanged(search_text_buf_);
    SyncSearchCaretFromEdit();
}

void Win32Window::FocusSearchEdit(WPARAM mode, LPARAM selection)
{
    if (!search_edit_) {
        return;
    }
    RepositionSearchEdit();
    SetFocus(search_edit_);
    if (mode == app_param::SEARCH_FOCUS_SET_CARET) {
        const auto pos = static_cast<int>(selection);
        SendMessageW(search_edit_, EM_SETSEL, pos, pos);
    }
    else if (mode == app_param::SEARCH_FOCUS_SET_SELECTION) {
        const auto [anchor, caret] = app_param::UnpackSearchSelectionLParam(selection);
        SendMessageW(search_edit_, EM_SETSEL, anchor, caret);
    }
    else {
        SendMessageW(search_edit_, EM_SETSEL, 0, -1);
    }
    SyncSearchCaretFromEdit();
}

void Win32Window::RepositionSearchEdit()
{
    if (!search_edit_ || !app_->IsSearchBarVisible()) {
        return;
    }
    const RECT rc = app_->GetSearchEditRect();
    SetWindowPos(search_edit_, nullptr, rc.left, rc.top, rc.right - rc.left, rc.bottom - rc.top, SWP_NOZORDER | SWP_NOACTIVATE);
}

void Win32Window::SyncSearchCaretFromEdit()
{
    if (!search_edit_) {
        return;
    }
    DWORD sel_start = 0;
    DWORD sel_end = 0;
    SendMessageW(search_edit_, EM_GETSEL, reinterpret_cast<WPARAM>(&sel_start), reinterpret_cast<LPARAM>(&sel_end));
    app_->SetSearchSelection(static_cast<int>(sel_start), static_cast<int>(sel_end));
}
