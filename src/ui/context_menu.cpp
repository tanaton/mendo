#include "context_menu_impl.h"
#include "d2d_util.h"
#include "theme.h"
#include "i18n.h"
#include "resource.h"
#include "ui_constants.h"
#include "utility.h"
#include "win32_util.h"
#include <algorithm>
#include <format>
#include <iterator>

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
    zoom = params.zoom;
    if (zoom) {
        items.emplace_back(ItemType::ZoomRow, 0, ls.menu_zoom);
        items.emplace_back(ItemType::Separator);
    }
    items.emplace_back(ItemType::Text, IDM_TOGGLE_DARK_MODE, ls.menu_dark_mode, true, params.dark_mode_checked);
    items.emplace_back(ItemType::Separator);
    items.emplace_back(ItemType::Text, IDM_TOGGLE_FILE_PANE, ls.menu_file_pane, true, params.file_pane_checked);
    items.emplace_back(ItemType::Text, IDM_TOGGLE_TOC_PANE, ls.menu_toc_pane, true, params.toc_pane_checked);
}

void ContextMenu::Impl::CreateTextFormats(const Theme& t)
{
    if (fmt_text && fmt_text_center && fmt_icon &&
        cached_fmt_font_family == std::wstring_view{ t.font_family } &&
        cached_fmt_icon_font == std::wstring_view{ t.icon_font } &&
        cached_fmt_font_size == t.pane_font_size) {
        return;
    }

    // 項目の文字は等倍のサイドペインの項目と同じ大きさにする。
    constexpr auto CENTER = DWRITE_PARAGRAPH_ALIGNMENT_CENTER;
    fmt_text = CreateTextFormat(dwrite_factory, { .family = t.font_family.c_str(), .size = t.pane_font_size, .locale = t.text_locale, .para_align = CENTER, .no_wrap = true });
    fmt_text_center = CreateTextFormat(dwrite_factory, { .family = t.font_family.c_str(), .size = t.pane_font_size, .locale = t.text_locale, .text_align = DWRITE_TEXT_ALIGNMENT_CENTER, .para_align = CENTER, .no_wrap = true });
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
    float zoom_row_w = 0.0f;
    for (const auto& item : items) {
        if (item.type == ItemType::ZoomRow) {
            zoom_row_w = PAD_X + TextWidth(item.text) + ZOOM_LABEL_GAP + ZOOM_CONTROLS_WIDTH + ZOOM_PAD_RIGHT;
        }
        if (item.type != ItemType::Text || item.text.empty()) {
            continue;
        }
        max_text_w = std::max(max_text_w, TextWidth(item.text));
    }

    const float nav_row_w = CTX_NAV_BUTTONS_WIDTH + 2 * PAD_X;
    const float text_w = CHECK_WIDTH + max_text_w + PAD_X * 2;
    menu_width = std::max({ nav_row_w, text_w, zoom_row_w, MIN_MENU_WIDTH });

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
        case ItemType::ZoomRow: {
            item.rect = { 0, y, menu_width, y + ITEM_HEIGHT };
            // 右端から 元に戻す・拡大・倍率・縮小 の順に詰める。
            const auto box = [y](float right, float width) { return DipRect{ right - width, y, right, y + ZOOM_BTN_SIZE }; };
            zoom_layout.reset_rect = box(menu_width - ZOOM_PAD_RIGHT, ZOOM_BTN_SIZE);
            zoom_layout.in_rect = box(zoom_layout.reset_rect.left - ZOOM_RESET_GAP, ZOOM_BTN_SIZE);
            zoom_layout.percent_rect = box(zoom_layout.in_rect.left, ZOOM_PERCENT_WIDTH);
            zoom_layout.out_rect = box(zoom_layout.percent_rect.left, ZOOM_BTN_SIZE);
            y += ITEM_HEIGHT;
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

std::optional<ZoomDirection> ContextMenu::Impl::ZoomHitTest(float x, float y) const noexcept
{
    if (!zoom) {
        return std::nullopt;
    }
    const std::pair<const DipRect&, ZoomDirection> buttons[] = {
        { zoom_layout.out_rect, ZoomDirection::Out },
        { zoom_layout.in_rect, ZoomDirection::In },
        { zoom_layout.reset_rect, ZoomDirection::Reset },
    };
    for (const auto& [rect, direction] : buttons) {
        if (zoom->CanStep(direction) && PointInRect(x, y, rect)) {
            return direction;
        }
    }
    return std::nullopt;
}

namespace {

constexpr wchar_t kContextMenuClass[] = L"mendoContextMenu";

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
    static const bool registered_ok = RegisterPopupWindowClass(kContextMenuClass, WndProc, CS_HREDRAW | CS_VREDRAW);
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

    const auto [x, y] = ClampToWorkArea(screen_x, screen_y, pixel_w, pixel_h);

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

    ApplyRoundedRegion(hwnd, pixel_w, pixel_h, MENU_CORNER, dpi_scale, /*redraw=*/false);

    selected_id = 0;
    done = false;
    hovered_id = 0;
    hovered_nav = 0;
    hovered_zoom.reset();

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

int ContextMenu::Show(HWND owner_hwnd, const ContextMenuParams& params)
{
    auto& s = *impl_;
    if (!s.d2d_factory || !s.dwrite_factory || !params.theme || params.dpi_scale <= 0.0f) {
        return 0;
    }

    s.owner = owner_hwnd;
    s.theme = params.theme;
    s.dpi_scale = params.dpi_scale;
    s.on_zoom = params.on_zoom;
    // theme と zoom は呼び出し側の借用ポインタ。Show 終了後にダングリングさせないよう必ず手放す。
    auto theme_guard = ScopeGuard([&s] {
        s.theme = nullptr;
        s.zoom = nullptr;
        s.on_zoom = nullptr;
    });

    s.BuildItems(params);
    s.CreateTextFormats(*s.theme);
    s.ComputeLayout();
    if (!s.CreatePopupWindow(params.screen_x, params.screen_y)) {
        return 0;
    }
    RunPopupModalLoop(s.done);

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
        const auto old_zoom = hovered_zoom;

        hovered_nav = NavHitTest(x, y);
        hovered_zoom = ZoomHitTest(x, y);
        hovered_id = hovered_nav == 0 && !hovered_zoom ? HitTest(x, y) : 0;

        if (hovered_id != old_hovered || hovered_nav != old_nav || hovered_zoom != old_zoom) {
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

        // ズームは閉じずに倍率だけ変え、続けて押せるようにする。端に着いたボタンは押せなくなる。
        if (const auto zoom_hit = ZoomHitTest(x, y)) {
            if (on_zoom) {
                on_zoom(*zoom_hit);
            }
            hovered_zoom = ZoomHitTest(x, y);
            InvalidateRect(hwnd, nullptr, FALSE);
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
        case ItemType::ZoomRow:
            DrawZoomRow(item);
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

void ContextMenu::Impl::DrawIconButton(const DipRect& rect, const wchar_t* glyph, bool enabled, bool hovered, float corner)
{
    const D2D1_RECT_F rc = ToD2DRect(rect);
    if (hovered) {
        rt->FillRoundedRectangle({ rc, corner, corner }, brush_hover.Get());
    }
    if (fmt_icon) {
        rt->DrawText(glyph, 1, fmt_icon.Get(), rc, enabled ? brush_text.Get() : brush_gray.Get());
    }
}

void ContextMenu::Impl::DrawNavRow()
{
    DrawIconButton(nav_layout.back_rect, GLYPH_BACK, nav_layout.back_enabled, hovered_nav == IDM_NAV_BACK, CTX_NAV_BTN_CORNER);
    DrawIconButton(nav_layout.fwd_rect, GLYPH_FORWARD, nav_layout.fwd_enabled, hovered_nav == IDM_NAV_FORWARD, CTX_NAV_BTN_CORNER);
}

void ContextMenu::Impl::DrawZoomRow(const Item& item)
{
    if (!zoom || !fmt_text || !fmt_text_center) {
        return;
    }
    const D2D1_RECT_F label_rc = { item.rect.left + PAD_X, item.rect.top, zoom_layout.out_rect.left, item.rect.bottom };
    rt->DrawText(item.text.data(), static_cast<UINT32>(item.text.size()), fmt_text.Get(), label_rc, brush_text.Get());

    const auto draw_btn = [&](const DipRect& rect, const wchar_t* glyph, ZoomDirection direction) {
        const bool enabled = zoom->CanStep(direction);
        DrawIconButton(rect, glyph, enabled, enabled && hovered_zoom == direction, HOVER_CORNER);
    };
    draw_btn(zoom_layout.out_rect, GLYPH_ZOOM_OUT, ZoomDirection::Out);
    draw_btn(zoom_layout.in_rect, GLYPH_ZOOM_IN, ZoomDirection::In);
    draw_btn(zoom_layout.reset_rect, GLYPH_ZOOM_RESET, ZoomDirection::Reset);

    wchar_t percent[16];
    const auto r = std::format_to_n(percent, std::size(percent), L"{}%", zoom->Percent());
    const UINT32 len = static_cast<UINT32>(std::min(static_cast<size_t>(r.size), std::size(percent)));
    rt->DrawText(percent, len, fmt_text_center.Get(), ToD2DRect(zoom_layout.percent_rect), brush_text.Get());
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
