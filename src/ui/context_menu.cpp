#include "context_menu_impl.h"
#include "d2d_util.h"
#include "theme.h"
#include "resource.h"
#include <cmath>
#include <mutex>

using Microsoft::WRL::ComPtr;
using namespace context_menu_constants;

bool ContextMenu::Impl::RegisterWindowClass()
{
    static std::once_flag flag;
    static bool registered_ok = false;
    std::call_once(flag, [] {
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.style = CS_HREDRAW | CS_VREDRAW;
        wc.lpfnWndProc = WndProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.lpszClassName = L"mendoContextMenu";
        registered_ok = (RegisterClassExW(&wc) != 0);
    });
    return registered_ok;
}

LRESULT CALLBACK ContextMenu::Impl::WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    ContextMenu::Impl* self = nullptr;
    if (msg == WM_NCCREATE) {
        const auto* cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
        self = static_cast<ContextMenu::Impl*>(cs->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        self->hwnd = hwnd;
    }
    else {
        self = reinterpret_cast<ContextMenu::Impl*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }
    if (self) {
        return self->HandleMessage(msg, wParam, lParam);
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

void ContextMenu::Impl::PrepareContent(const ContextMenuParams& params)
{
    BuildItems(params);
    CreateTextFormats(*theme);
    ComputeLayout();
}

bool ContextMenu::Impl::CreatePopupWindow(int screen_x, int screen_y)
{
    if (!RegisterWindowClass()) {
        return false;
    }
    if (hwnd) {
        DestroyWindow(hwnd);
        hwnd = nullptr;
    }
    rt.Reset();

    const int pixel_w = static_cast<int>(std::ceil(menu_width * dpi_scale));
    const int pixel_h = static_cast<int>(std::ceil(menu_height * dpi_scale));

    // 画面外にはみ出さないよう調整
    const HMONITOR monitor = MonitorFromPoint({ screen_x, screen_y }, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi = { sizeof(mi) };
    GetMonitorInfoW(monitor, &mi);

    int x = screen_x;
    int y = screen_y;
    if (x + pixel_w > mi.rcWork.right) {
        x = mi.rcWork.right - pixel_w;
    }
    if (y + pixel_h > mi.rcWork.bottom) {
        y = mi.rcWork.bottom - pixel_h;
    }
    if (x < mi.rcWork.left) {
        x = mi.rcWork.left;
    }
    if (y < mi.rcWork.top) {
        y = mi.rcWork.top;
    }

    hwnd = CreateWindowExW(
        WS_EX_TOOLWINDOW | WS_EX_TOPMOST,
        L"mendoContextMenu", nullptr,
        WS_POPUP,
        x, y, pixel_w, pixel_h,
        owner, nullptr, GetModuleHandleW(nullptr), this);

    if (!hwnd) {
        return false;
    }

    const float dpi = dpi_scale * 96.0f;
    if (!EnsureRenderTarget(dpi)) {
        DestroyWindow(hwnd);
        hwnd = nullptr;
        return false;
    }
    CreateBrushes();

    // 角丸クリッピング用リージョン
    // SetWindowRgn は成功時のみ rgn の所有権を OS に移譲する。失敗時は呼び出し側で DeleteObject。
    const int corner_px = static_cast<int>(MENU_CORNER * dpi_scale);
    const HRGN rgn = CreateRoundRectRgn(0, 0, pixel_w + 1, pixel_h + 1, corner_px, corner_px);
    if (rgn && !SetWindowRgn(hwnd, rgn, FALSE)) {
        DeleteObject(rgn);
    }

    selected_id = 0;
    done = false;
    hovered_id = 0;
    hovered_nav = 0;

    ShowWindow(hwnd, SW_SHOW);
    SetForegroundWindow(hwnd);
    SetCapture(hwnd);
    return true;
}

void ContextMenu::Impl::RunModalLoop()
{
    MSG msg{};
    while (!done) {
        // WM_APP+* (TaskScheduler 完了通知など) を parent_hwnd 宛のままキューに残し、メインループで処理させる。
        // メニュー表示中に App 状態を書き換えると race になるため。
        if (PeekMessageW(&msg, nullptr, WM_QUIT, WM_QUIT, PM_NOREMOVE)) {
            PostQuitMessage(static_cast<int>(msg.wParam));
            done = true;
            break;
        }
        if (!PeekMessageW(&msg, hwnd, 0, 0, PM_REMOVE)) {
            WaitMessage();
            continue;
        }
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
}

int ContextMenu::Show(HWND owner_hwnd, const ContextMenuParams& params)
{
    auto& s = *impl_;
    if (!s.d2d_factory || !s.dwrite_factory || !params.theme || params.dpi_scale <= 0.0f) {
        return 0;
    }

    s.owner = owner_hwnd;
    s.theme = params.theme;
    s.dpi_scale = params.dpi_scale;

    s.PrepareContent(params);
    if (!s.CreatePopupWindow(params.screen_x, params.screen_y)) {
        s.theme = nullptr;
        return 0;
    }
    s.RunModalLoop();

    if (s.hwnd) {
        ReleaseCapture();
        DestroyWindow(s.hwnd);
        s.hwnd = nullptr;
    }
    s.rt.Reset();
    s.theme = nullptr;

    return s.selected_id;
}

LRESULT ContextMenu::Impl::HandleMessage(UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg) {
    case WM_PAINT: {
        PAINTSTRUCT ps;
        BeginPaint(hwnd, &ps);
        Paint();
        EndPaint(hwnd, &ps);
        return 0;
    }

    case WM_MOUSEMOVE: {
        const float x = static_cast<short>(LOWORD(lParam)) / dpi_scale;
        const float y = static_cast<short>(HIWORD(lParam)) / dpi_scale;

        const int old_hovered = hovered_id;
        const int old_nav = hovered_nav;

        hovered_id = 0;
        hovered_nav = 0;

        if (!items.empty() && items[0].type == ItemType::NavRow) {
            if (nav_layout.back_enabled && PointInRect(x, y, nav_layout.back_rect)) {
                hovered_nav = -1;
            }
            else if (nav_layout.fwd_enabled && PointInRect(x, y, nav_layout.fwd_rect)) {
                hovered_nav = 1;
            }
        }

        if (hovered_nav == 0) {
            hovered_id = HitTest(x, y);
        }

        if (hovered_id != old_hovered || hovered_nav != old_nav) {
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        return 0;
    }

    case WM_LBUTTONDOWN:
    case WM_RBUTTONDOWN: {
        const float x = static_cast<short>(LOWORD(lParam)) / dpi_scale;
        const float y = static_cast<short>(HIWORD(lParam)) / dpi_scale;

        if (x < 0 || y < 0 || x >= menu_width || y >= menu_height) {
            done = true;
            return 0;
        }

        const int nav_hit = NavHitTest(x, y);
        if (nav_hit != 0) {
            selected_id = nav_hit;
            done = true;
            return 0;
        }

        const int hit = HitTest(x, y);
        for (const auto& item : items) {
            if (item.id == hit && item.enabled) {
                selected_id = hit;
                done = true;
                return 0;
            }
        }
        return 0;
    }

    case WM_KEYDOWN:
        if (wParam == VK_ESCAPE) {
            done = true;
        }
        return 0;

    case WM_CAPTURECHANGED:
        if (reinterpret_cast<HWND>(lParam) != hwnd) {
            done = true;
        }
        return 0;

    case WM_KILLFOCUS:
        done = true;
        return 0;

    case WM_ACTIVATEAPP:
        if (wParam == FALSE) {
            done = true;
        }
        return 0;

    case WM_DESTROY:
        return 0;

    default:
        return DefWindowProcW(hwnd, msg, wParam, lParam);
    }
}

bool ContextMenu::Impl::EnsureRenderTarget(float dpi)
{
    if (rt) {
        return true;
    }
    RECT rc{};
    if (!GetClientRect(hwnd, &rc)) {
        return false;
    }
    const D2D1_SIZE_U size{ static_cast<UINT32>(rc.right), static_cast<UINT32>(rc.bottom) };
    D2D1_RENDER_TARGET_PROPERTIES rtProps = D2D1::RenderTargetProperties();
    rtProps.dpiX = dpi;
    rtProps.dpiY = dpi;
    const auto hwndProps = D2D1::HwndRenderTargetProperties(hwnd, size);
    const HRESULT hr = d2d_factory->CreateHwndRenderTarget(rtProps, hwndProps, &rt);
    return SUCCEEDED(hr);
}

void ContextMenu::Impl::CreateBrushes()
{
    if (!rt || !theme) {
        return;
    }
    auto make = [&](D2D1_COLOR_F c) {
        ComPtr<ID2D1SolidColorBrush> b;
        mendo::CreateSolidColorBrushOrFallback(rt.Get(), c, b);
        return b;
    };

    brush_border = make(theme->splitter_color);
    brush_text = make(theme->text_color);

    auto gray = theme->text_color;
    gray.a = 0.35f;
    brush_gray = make(gray);

    brush_hover = make(theme->pane_item_hover_color);
    brush_check = make(theme->link_color);
}

void ContextMenu::Impl::Paint()
{
    if (!rt) {
        return;
    }
    rt->BeginDraw();
    rt->Clear(theme->pane_bg_color);

    const D2D1_RECT_F border_rect{
        MENU_BORDER * 0.5f,
        MENU_BORDER * 0.5f,
        menu_width - MENU_BORDER * 0.5f,
        menu_height - MENU_BORDER * 0.5f
    };
    const D2D1_ROUNDED_RECT rr{ border_rect, MENU_CORNER, MENU_CORNER };
    rt->DrawRoundedRectangle(rr, brush_border.Get(), MENU_BORDER);

    for (const auto& item : items) {
        switch (item.type) {
        case ItemType::NavRow:
            DrawNavRow();
            break;
        case ItemType::Separator:
            DrawSeparator(item);
            break;
        case ItemType::Text:
            DrawTextItem(item);
            break;
        }
    }

    const HRESULT hr = rt->EndDraw();
    if (hr == D2DERR_RECREATE_TARGET) {
        rt.Reset();
    }
}

void ContextMenu::Impl::DrawNavRow()
{
    auto draw_btn = [&](const DipRect& dr, const wchar_t* glyph, bool enabled, bool hovered) {
        const D2D1_RECT_F rc{ dr.left, dr.top, dr.right, dr.bottom };
        if (hovered) {
            const D2D1_ROUNDED_RECT rr = { rc, NAV_BTN_CORNER, NAV_BTN_CORNER };
            rt->FillRoundedRectangle(rr, brush_hover.Get());
        }
        auto* brush = enabled ? brush_text.Get() : brush_gray.Get();
        if (fmt_icon) {
            rt->DrawText(glyph, 1, fmt_icon.Get(), rc, brush);
        }
    };

    draw_btn(nav_layout.back_rect, GLYPH_BACK, nav_layout.back_enabled, hovered_nav == -1);
    draw_btn(nav_layout.fwd_rect, GLYPH_FORWARD, nav_layout.fwd_enabled, hovered_nav == 1);
}

void ContextMenu::Impl::DrawSeparator(const Item& item)
{
    const float cy = (item.rect.top + item.rect.bottom) / 2.0f;
    const float margin = 12.0f;
    rt->DrawLine(
        { item.rect.left + margin, cy },
        { item.rect.right - margin, cy },
        brush_border.Get(),
        1.0f);
}

void ContextMenu::Impl::DrawTextItem(const Item& item)
{
    const bool hovered = (item.id != 0 && item.id == hovered_id && item.enabled);

    if (hovered) {
        const float margin = 4.0f;
        const D2D1_ROUNDED_RECT rr{
            { item.rect.left + margin,
             item.rect.top + 1.0f,
             item.rect.right - margin,
             item.rect.bottom - 1.0f },
            4.0f,
            4.0f
        };
        rt->FillRoundedRectangle(rr, brush_hover.Get());
    }

    auto* brush = item.enabled ? brush_text.Get() : brush_gray.Get();

    if (item.checked) {
        const D2D1_RECT_F check_rc = {
            item.rect.left + 8.0f, item.rect.top,
            item.rect.left + CHECK_WIDTH + 4.0f, item.rect.bottom
        };
        if (fmt_icon) {
            rt->DrawText(GLYPH_CHECKMARK, 1, fmt_icon.Get(), check_rc, brush_check.Get());
        }
    }

    if (fmt_text && !item.text.empty()) {
        const D2D1_RECT_F text_rc = {
            item.rect.left + PAD_X, item.rect.top,
            item.rect.right - 8.0f, item.rect.bottom
        };
        rt->DrawText(item.text.data(), static_cast<UINT32>(item.text.size()), fmt_text.Get(), text_rc, brush);
    }
}
