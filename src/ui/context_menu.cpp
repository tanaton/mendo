#include "context_menu_impl.h"
#include "d2d_util.h"
#include "theme.h"
#include "i18n.h"
#include "resource.h"
#include "ui_constants.h"
#include "utility.h"
#include "win32_util.h"
#include <algorithm>
#include <mutex>
#include <windowsx.h>

using Microsoft::WRL::ComPtr;
using namespace context_menu_constants;

ContextMenu::ContextMenu() : impl_(std::make_unique<Impl>())
{}
ContextMenu::~ContextMenu() = default;

void ContextMenu::Init(ID2D1Factory* d2d_factory, IDWriteFactory* dwrite_factory)
{
    impl_->d2d_factory = d2d_factory;
    impl_->dwrite_factory = dwrite_factory;
}

void ContextMenu::Impl::BuildItems(const ContextMenuParams& params)
{
    items.clear();

    items.emplace_back(ItemType::NavRow);
    nav_layout.back_enabled = params.can_go_back;
    nav_layout.fwd_enabled = params.can_go_forward;

    items.emplace_back(ItemType::Separator);

    const auto& ls = i18n::S();
    if (params.show_file_items) {
        items.emplace_back(ItemType::Text, IDM_EDIT_FILE, ls.menu_edit_file, params.has_file, false);
        items.emplace_back(ItemType::Text, IDM_COPY, ls.menu_copy, params.has_selection, false);
        items.emplace_back(ItemType::Text, IDM_COPY_FORMATTED, ls.menu_copy_formatted, params.has_selection, false);
        items.emplace_back(ItemType::Separator);
    }
    items.emplace_back(ItemType::Text, IDM_TOGGLE_DARK_MODE, ls.menu_dark_mode, true, params.dark_mode_checked);
    items.emplace_back(ItemType::Separator);
    items.emplace_back(ItemType::Text, IDM_TOGGLE_FILE_PANE, ls.menu_file_pane, true, params.file_pane_checked);
    items.emplace_back(ItemType::Text, IDM_TOGGLE_TOC_PANE, ls.menu_toc_pane, true, params.toc_pane_checked);
}

void ContextMenu::Impl::CreateTextFormats(const Theme& t)
{
    if (fmt_text && fmt_icon &&
        cached_fmt_font_family == std::wstring_view{ t.font_family } &&
        cached_fmt_icon_font == std::wstring_view{ t.icon_font } &&
        cached_fmt_font_size == t.pane_font_size) {
        return;
    }

    // 項目の文字はサイドペインの項目 (Renderer の pane_item) と同じ書式にする。
    constexpr auto CENTER = DWRITE_PARAGRAPH_ALIGNMENT_CENTER;
    fmt_text = CreateTextFormat(dwrite_factory, { .family = t.font_family.c_str(), .size = t.pane_font_size, .para_align = CENTER, .no_wrap = true });
    fmt_icon = CreateTextFormat(dwrite_factory, { .family = t.icon_font.c_str(), .size = ICON_FONT_SIZE, .text_align = DWRITE_TEXT_ALIGNMENT_CENTER, .para_align = CENTER, .no_wrap = true });

    cached_fmt_font_family.assign(t.font_family.begin(), t.font_family.end());
    cached_fmt_icon_font.assign(t.icon_font.begin(), t.icon_font.end());
    cached_fmt_font_size = t.pane_font_size;
    text_widths.clear();
}

float ContextMenu::Impl::TextWidth(std::wstring_view text)
{
    if (const auto it = text_widths.find(text.data()); it != text_widths.end()) {
        return it->second;
    }
    float width = 0.0f;
    ComPtr<IDWriteTextLayout> layout;
    dwrite_factory->CreateTextLayout(
        text.data(), static_cast<UINT32>(text.size()),
        fmt_text.Get(), 1000.0f, 100.0f, &layout);
    if (layout) {
        DWRITE_TEXT_METRICS metrics{};
        layout->GetMetrics(&metrics);
        width = metrics.width;
        text_widths.emplace(text.data(), width);
    }
    return width;
}

void ContextMenu::Impl::ComputeLayout()
{
    float max_text_w = 0.0f;
    for (const auto& item : items) {
        if (item.type != ItemType::Text || item.text.empty()) {
            continue;
        }
        max_text_w = std::max(max_text_w, TextWidth(item.text));
    }

    const float nav_row_w = CTX_NAV_BUTTONS_WIDTH + 2 * PAD_X;
    const float text_w = CHECK_WIDTH + max_text_w + PAD_X * 2;
    menu_width = std::max({ nav_row_w, text_w, MIN_MENU_WIDTH });

    float y = PAD_Y;
    for (auto& item : items) {
        switch (item.type) {
        case ItemType::NavRow: {
            const float row_h = CTX_NAV_BTN_SIZE + 2 * CTX_NAV_ROW_PAD_Y;
            item.rect = { 0, y, menu_width, y + row_h };

            const float cx = menu_width / 2.0f;
            const float bx = cx - CTX_NAV_BUTTONS_WIDTH / 2.0f;
            const float by = y + CTX_NAV_ROW_PAD_Y;
            nav_layout.back_rect = { bx, by, bx + CTX_NAV_BTN_SIZE, by + CTX_NAV_BTN_SIZE };
            nav_layout.fwd_rect = { bx + CTX_NAV_BTN_SIZE + CTX_NAV_BTN_GAP, by, bx + CTX_NAV_BUTTONS_WIDTH, by + CTX_NAV_BTN_SIZE };
            y += row_h;
            break;
        }
        case ItemType::Separator:
            item.rect = { 0, y, menu_width, y + SEPARATOR_HEIGHT };
            y += SEPARATOR_HEIGHT;
            break;
        case ItemType::Text:
            item.rect = { 0, y, menu_width, y + ITEM_HEIGHT };
            y += ITEM_HEIGHT;
            break;
        default:
            std::unreachable();
        }
    }

    menu_height = y + PAD_Y;
}

int ContextMenu::Impl::HitTest(float x, float y) const noexcept
{
    for (const auto& item : items) {
        if (item.type != ItemType::Text || item.id == 0 || !item.enabled) {
            continue;
        }
        if (PointInRect(x, y, item.rect)) {
            return item.id;
        }
    }
    return 0;
}

int ContextMenu::Impl::NavHitTest(float x, float y) const noexcept
{
    if (nav_layout.back_enabled && PointInRect(x, y, nav_layout.back_rect)) {
        return IDM_NAV_BACK;
    }
    if (nav_layout.fwd_enabled && PointInRect(x, y, nav_layout.fwd_rect)) {
        return IDM_NAV_FORWARD;
    }
    return 0;
}

namespace {

constexpr wchar_t kContextMenuClass[] = L"mendoContextMenu";

DipPoint DipPointFromLParam(LPARAM lParam, float dpi_scale) noexcept
{
    return PixelToDip(GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam), dpi_scale);
}

} // namespace

void ContextMenu::Impl::DestroyPopupWindow() noexcept
{
    if (hwnd) {
        DestroyWindow(hwnd);
        hwnd = nullptr;
    }
    rt.Reset();
}

bool ContextMenu::Impl::RegisterWindowClass()
{
    static std::once_flag flag;
    static bool registered_ok = false;
    std::call_once(flag, [] static {
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.style = CS_HREDRAW | CS_VREDRAW;
        wc.lpfnWndProc = WndProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.lpszClassName = kContextMenuClass;
        registered_ok = (RegisterClassExW(&wc) != 0);
    });
    return registered_ok;
}

LRESULT CALLBACK ContextMenu::Impl::WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    auto* self = BindWindowInstance<ContextMenu::Impl>(hwnd, msg, lParam);
    if (!self) {
        return DefWindowProcW(hwnd, msg, wParam, lParam);
    }
    if (msg == WM_NCCREATE) {
        self->hwnd = hwnd;
    }
    return self->HandleMessage(msg, wParam, lParam);
}

bool ContextMenu::Impl::CreatePopupWindow(int screen_x, int screen_y)
{
    if (!RegisterWindowClass()) {
        return false;
    }
    if (hwnd && GetWindow(hwnd, GW_OWNER) != owner) {
        DestroyPopupWindow();
    }

    const int pixel_w = DipToPixelCeil(menu_width, dpi_scale);
    const int pixel_h = DipToPixelCeil(menu_height, dpi_scale);

    const HMONITOR monitor = MonitorFromPoint({ screen_x, screen_y }, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi = { sizeof(mi) };
    GetMonitorInfoW(monitor, &mi);

    // 作業領域より大きいメニューは左上端を優先する (std::clamp は lo > hi で使えない)。
    const int x = std::max(std::min(screen_x, static_cast<int>(mi.rcWork.right) - pixel_w), static_cast<int>(mi.rcWork.left));
    const int y = std::max(std::min(screen_y, static_cast<int>(mi.rcWork.bottom) - pixel_h), static_cast<int>(mi.rcWork.top));

    if (hwnd) {
        SetWindowPos(hwnd, HWND_TOPMOST, x, y, pixel_w, pixel_h, SWP_NOACTIVATE);
    }
    else {
        hwnd = CreateWindowExW(
            WS_EX_TOOLWINDOW | WS_EX_TOPMOST,
            kContextMenuClass, nullptr,
            WS_POPUP,
            x, y, pixel_w, pixel_h,
            owner, nullptr, GetModuleHandleW(nullptr), this);
        if (!hwnd) {
            return false;
        }
    }

    if (!PrepareDeviceResources(static_cast<UINT32>(pixel_w), static_cast<UINT32>(pixel_h))) {
        DestroyPopupWindow();
        return false;
    }

    // SetWindowRgn は成功時のみ rgn の所有権を OS に移譲する。失敗時は呼び出し側で DeleteObject。
    const int corner_px = DipToPixel(MENU_CORNER, dpi_scale);
    const HRGN rgn = CreateRoundRectRgn(0, 0, pixel_w + 1, pixel_h + 1, corner_px, corner_px);
    if (rgn && !SetWindowRgn(hwnd, rgn, FALSE)) {
        DeleteObject(rgn);
    }

    selected_id = 0;
    done = false;
    hovered_id = 0;
    hovered_nav = 0;

    // 使い回したウィンドウには前回の絵が残っているので、見せたらすぐ描き直す。
    InvalidateRect(hwnd, nullptr, FALSE);
    ShowWindow(hwnd, SW_SHOW);
    UpdateWindow(hwnd);
    SetForegroundWindow(hwnd);
    SetCapture(hwnd);
    return true;
}

void ContextMenu::Impl::HidePopupWindow() noexcept
{
    if (!hwnd) {
        return;
    }
    // 壊すときと違い、隠すだけでは前面がオーナーへ戻らない。他のアプリへ移って閉じたときは奪わない。
    if (GetForegroundWindow() == hwnd) {
        SetForegroundWindow(owner);
    }
    ShowWindow(hwnd, SW_HIDE);
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
    // theme は呼び出し側 Theme の借用ポインタ。Show 終了後にダングリングさせないよう必ず手放す。
    auto theme_guard = ScopeGuard([&s] { s.theme = nullptr; });

    s.BuildItems(params);
    s.CreateTextFormats(*s.theme);
    s.ComputeLayout();
    if (!s.CreatePopupWindow(params.screen_x, params.screen_y)) {
        return 0;
    }
    s.RunModalLoop();

    if (s.hwnd) {
        ReleaseCapture();
    }
    s.HidePopupWindow();

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
        const auto [x, y] = DipPointFromLParam(lParam, dpi_scale);
        const int old_hovered = hovered_id;
        const int old_nav = hovered_nav;

        hovered_nav = NavHitTest(x, y);
        hovered_id = hovered_nav == 0 ? HitTest(x, y) : 0;

        if (hovered_id != old_hovered || hovered_nav != old_nav) {
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        return 0;
    }

    case WM_LBUTTONDOWN:
    case WM_RBUTTONDOWN: {
        const auto [x, y] = DipPointFromLParam(lParam, dpi_scale);
        if (!PointInRect(x, y, DipRect{ 0.0f, 0.0f, menu_width, menu_height })) {
            done = true;
            return 0;
        }

        const int nav_hit = NavHitTest(x, y);
        const int hit = nav_hit != 0 ? nav_hit : HitTest(x, y);
        if (hit != 0) {
            selected_id = hit;
            done = true;
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

    case WM_NCDESTROY: {
        // オーナーと一緒に壊されたときも、使い回すつもりで壊れたハンドルを持ち続けない。
        const HWND destroyed = std::exchange(hwnd, nullptr);
        rt.Reset();
        return DefWindowProcW(destroyed, msg, wParam, lParam);
    }

    default:
        return DefWindowProcW(hwnd, msg, wParam, lParam);
    }
}

bool ContextMenu::Impl::PrepareDeviceResources(UINT32 pixel_w, UINT32 pixel_h)
{
    if (!rt) {
        return CreateDeviceResources();
    }
    rt->SetDpi(dpi_scale * DEFAULT_DPI, dpi_scale * DEFAULT_DPI);
    if (FAILED(rt->Resize(D2D1_SIZE_U{ pixel_w, pixel_h }))) {
        rt.Reset();
        return CreateDeviceResources();
    }
    ApplyBrushColors();
    return true;
}

void ContextMenu::Impl::ApplyBrushColors()
{
    auto gray = theme->text_color;
    gray.a = DISABLED_UI_ALPHA;
    const std::pair<ID2D1SolidColorBrush*, D2D1_COLOR_F> colors[] = {
        { brush_border.Get(), theme->splitter_color        },
        { brush_text.Get(),   theme->text_color            },
        { brush_gray.Get(),   gray                         },
        { brush_hover.Get(),  theme->pane_item_hover_color },
        { brush_check.Get(),  theme->link_color            },
    };
    for (const auto& [brush, color] : colors) {
        if (brush) {
            brush->SetColor(color);
        }
    }
}

bool ContextMenu::Impl::CreateDeviceResources()
{
    RECT rc{};
    if (!GetClientRect(hwnd, &rc)) {
        return false;
    }
    const D2D1_SIZE_U size{ static_cast<UINT32>(rc.right), static_cast<UINT32>(rc.bottom) };
    // D3D デバイスの生成を伴うハードウェアは使わない。数百 px ならソフトウェアで足りる。
    D2D1_RENDER_TARGET_PROPERTIES rtProps = D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_SOFTWARE);
    rtProps.dpiX = dpi_scale * DEFAULT_DPI;
    rtProps.dpiY = dpi_scale * DEFAULT_DPI;
    const auto hwndProps = D2D1::HwndRenderTargetProperties(hwnd, size);
    if (FAILED(d2d_factory->CreateHwndRenderTarget(rtProps, hwndProps, &rt))) {
        return false;
    }

    for (auto* brush : { std::addressof(brush_border), std::addressof(brush_text), std::addressof(brush_gray), std::addressof(brush_hover), std::addressof(brush_check) }) {
        CreateSolidColorBrushOrFallback(rt.Get(), theme->text_color, *brush);
    }
    ApplyBrushColors();
    return true;
}

void ContextMenu::Impl::Paint()
{
    // 隠している間はテーマを借りていない。
    if (!theme) {
        return;
    }
    // デバイスロスト後 (EndDraw が D2DERR_RECREATE_TARGET で rt を破棄) はここで再生成する。
    if (!rt && !CreateDeviceResources()) {
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
        default:
            std::unreachable();
        }
    }

    const HRESULT hr = rt->EndDraw();
    if (hr == D2DERR_RECREATE_TARGET) {
        rt.Reset();
        // 次の WM_PAINT で rt を再生成させる。
        InvalidateRect(hwnd, nullptr, FALSE);
    }
}

void ContextMenu::Impl::DrawNavRow()
{
    auto draw_btn = [&](const DipRect& dr, const wchar_t* glyph, bool enabled, bool hovered) {
        const D2D1_RECT_F rc = ToD2DRect(dr);
        if (hovered) {
            const D2D1_ROUNDED_RECT rr = { rc, CTX_NAV_BTN_CORNER, CTX_NAV_BTN_CORNER };
            rt->FillRoundedRectangle(rr, brush_hover.Get());
        }
        auto* brush = enabled ? brush_text.Get() : brush_gray.Get();
        if (fmt_icon) {
            rt->DrawText(glyph, 1, fmt_icon.Get(), rc, brush);
        }
    };

    draw_btn(nav_layout.back_rect, GLYPH_BACK, nav_layout.back_enabled, hovered_nav == IDM_NAV_BACK);
    draw_btn(nav_layout.fwd_rect, GLYPH_FORWARD, nav_layout.fwd_enabled, hovered_nav == IDM_NAV_FORWARD);
}

void ContextMenu::Impl::DrawSeparator(const Item& item)
{
    const float cy = (item.rect.top + item.rect.bottom) / 2.0f;
    rt->DrawLine(
        { item.rect.left + SEPARATOR_MARGIN_X, cy },
        { item.rect.right - SEPARATOR_MARGIN_X, cy },
        brush_border.Get(),
        1.0f);
}

void ContextMenu::Impl::DrawTextItem(const Item& item)
{
    const bool hovered = item.id != 0 && item.id == hovered_id;

    if (hovered) {
        const D2D1_ROUNDED_RECT rr{
            { item.rect.left + HOVER_MARGIN_X,
             item.rect.top + 1.0f,
             item.rect.right - HOVER_MARGIN_X,
             item.rect.bottom - 1.0f },
            HOVER_CORNER,
            HOVER_CORNER
        };
        rt->FillRoundedRectangle(rr, brush_hover.Get());
    }

    auto* brush = item.enabled ? brush_text.Get() : brush_gray.Get();

    if (item.checked && fmt_icon) {
        const D2D1_RECT_F check_rc = {
            item.rect.left + CHECK_LEFT, item.rect.top,
            item.rect.left + CHECK_WIDTH + 4.0f, item.rect.bottom
        };
        rt->DrawText(GLYPH_CHECKMARK, 1, fmt_icon.Get(), check_rc, brush_check.Get());
    }

    if (fmt_text && !item.text.empty()) {
        const D2D1_RECT_F text_rc = {
            item.rect.left + PAD_X, item.rect.top,
            item.rect.right - TEXT_PAD_RIGHT, item.rect.bottom
        };
        rt->DrawText(item.text.data(), static_cast<UINT32>(item.text.size()), fmt_text.Get(), text_rc, brush);
    }
}
