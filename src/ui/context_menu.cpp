#include "context_menu.h"
#include "theme.h"
#include "i18n.h"
#include "resource.h"
#include <d2d1.h>
#include <dwrite.h>
#include <wrl/client.h>
#include <windows.h>
#include <cmath>

using Microsoft::WRL::ComPtr;

namespace {
constexpr float ITEM_HEIGHT = 28.0f;
constexpr float NAV_BTN_SIZE = 28.0f;
constexpr float NAV_BTN_GAP = 16.0f;
constexpr float NAV_ROW_PAD_Y = 5.0f;
constexpr float NAV_BTN_CORNER = 4.0f;
constexpr float SEPARATOR_HEIGHT = 9.0f;
constexpr float PAD_X = 28.0f;
constexpr float PAD_Y = 4.0f;
constexpr float CHECK_WIDTH = 20.0f;
constexpr float MENU_CORNER = 8.0f;
constexpr float MENU_BORDER = 1.0f;

constexpr wchar_t GLYPH_BACK[] = L"\xE72B";
constexpr wchar_t GLYPH_FORWARD[] = L"\xE72A";
constexpr wchar_t GLYPH_CHECKMARK[] = L"\xE73E";
} // namespace

struct ContextMenu::Impl {
    // ウィンドウクラス登録フラグ
    static bool class_registered;
    static bool RegisterWindowClass();
    static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
    LRESULT HandleMessage(UINT msg, WPARAM wParam, LPARAM lParam);

    void BuildItems(const ContextMenuParams& params);
    void ComputeLayout();
    bool EnsureRenderTarget(float dpi);
    void CreateBrushes();
    void CreateTextFormats(const Theme& theme);

    void Paint();
    void DrawNavRow();
    void DrawSeparator(const Item& item);
    void DrawTextItem(const Item& item);

    int HitTest(float x, float y) const noexcept;
    int NavHitTest(float x, float y) const noexcept;

    // ウィンドウ
    HWND hwnd = nullptr;
    HWND owner = nullptr;

    // モーダルループ制御
    bool done = false;
    int selected_id = 0;

    // ホバー状態
    int hovered_id = 0;
    int hovered_nav = 0;

    // メニュー項目
    std::vector<Item> items;
    NavRowLayout nav_layout{};
    float menu_width = 0.0f;
    float menu_height = 0.0f;

    // D2Dリソース
    ID2D1Factory* d2d_factory = nullptr;
    IDWriteFactory* dwrite_factory = nullptr;
    ComPtr<ID2D1HwndRenderTarget> rt;
    ComPtr<ID2D1SolidColorBrush> brush_border;
    ComPtr<ID2D1SolidColorBrush> brush_text;
    ComPtr<ID2D1SolidColorBrush> brush_gray;
    ComPtr<ID2D1SolidColorBrush> brush_hover;
    ComPtr<ID2D1SolidColorBrush> brush_check;
    ComPtr<IDWriteTextFormat> fmt_text;
    ComPtr<IDWriteTextFormat> fmt_icon;

    const Theme* theme = nullptr;
    float dpi_scale = 1.0f;

    ~Impl()
    {
        if (hwnd) {
            DestroyWindow(hwnd);
            hwnd = nullptr;
        }
    }
};

bool ContextMenu::Impl::class_registered = false;

// ============================================================
// 公開 API（PIMPL 経由の forward）
// ============================================================

ContextMenu::ContextMenu() : impl_(std::make_unique<Impl>()) {}
ContextMenu::~ContextMenu() = default;

void ContextMenu::Init(ID2D1Factory* d2d_factory, IDWriteFactory* dwrite_factory)
{
    impl_->d2d_factory = d2d_factory;
    impl_->dwrite_factory = dwrite_factory;
}

int ContextMenu::HitTest(float x, float y) const noexcept
{
    return impl_->HitTest(x, y);
}

int ContextMenu::NavHitTest(float x, float y) const noexcept
{
    return impl_->NavHitTest(x, y);
}

const std::vector<ContextMenu::Item>& ContextMenu::GetItems() const noexcept
{
    return impl_->items;
}

const ContextMenu::NavRowLayout& ContextMenu::GetNavLayout() const noexcept
{
    return impl_->nav_layout;
}

float ContextMenu::GetMenuWidth() const noexcept
{
    return impl_->menu_width;
}

float ContextMenu::GetMenuHeight() const noexcept
{
    return impl_->menu_height;
}

#ifdef MENDO_TESTING
void ContextMenu::TestBuildItems(const ContextMenuParams& params) { impl_->BuildItems(params); }
void ContextMenu::TestCreateTextFormats(const Theme& theme) { impl_->CreateTextFormats(theme); }
void ContextMenu::TestComputeLayout() { impl_->ComputeLayout(); }
#endif

// ============================================================
// ウィンドウクラス登録
// ============================================================

bool ContextMenu::Impl::RegisterWindowClass()
{
    if (class_registered) {
        return true;
    }
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = L"mendoContextMenu";
    if (!RegisterClassExW(&wc)) {
        return false;
    }
    class_registered = true;
    return true;
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

// ============================================================
// メニュー表示（モーダル）
// ============================================================

int ContextMenu::Show(HWND owner_hwnd, const ContextMenuParams& params)
{
    auto& s = *impl_;
    if (!s.d2d_factory || !s.dwrite_factory || !params.theme || params.dpi_scale <= 0.0f) {
        return 0;
    }

    s.owner = owner_hwnd;
    s.theme = params.theme;
    s.dpi_scale = params.dpi_scale;

    s.BuildItems(params);
    s.CreateTextFormats(*s.theme);
    s.ComputeLayout();

    if (!Impl::RegisterWindowClass()) {
        return 0;
    }
    if (s.hwnd) {
        DestroyWindow(s.hwnd);
        s.hwnd = nullptr;
    }
    s.rt.Reset();

    const int pixel_w = static_cast<int>(std::ceil(s.menu_width * s.dpi_scale));
    const int pixel_h = static_cast<int>(std::ceil(s.menu_height * s.dpi_scale));

    // 画面外にはみ出さないよう調整
    const HMONITOR monitor = MonitorFromPoint({ params.screen_x, params.screen_y }, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi = { sizeof(mi) };
    GetMonitorInfoW(monitor, &mi);

    int x = params.screen_x;
    int y = params.screen_y;
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

    s.hwnd = CreateWindowExW(
        WS_EX_TOOLWINDOW | WS_EX_TOPMOST,
        L"mendoContextMenu", nullptr,
        WS_POPUP,
        x, y, pixel_w, pixel_h,
        s.owner, nullptr, GetModuleHandleW(nullptr), &s);

    if (!s.hwnd) {
        return 0;
    }

    const float dpi = s.dpi_scale * 96.0f;
    if (!s.EnsureRenderTarget(dpi)) {
        DestroyWindow(s.hwnd);
        s.hwnd = nullptr;
        return 0;
    }
    s.CreateBrushes();

    // 角丸クリッピング用リージョン
    const int corner_px = static_cast<int>(MENU_CORNER * s.dpi_scale);
    const HRGN rgn = CreateRoundRectRgn(0, 0, pixel_w + 1, pixel_h + 1, corner_px, corner_px);
    SetWindowRgn(s.hwnd, rgn, FALSE);

    s.selected_id = 0;
    s.done = false;
    s.hovered_id = 0;
    s.hovered_nav = 0;

    ShowWindow(s.hwnd, SW_SHOW);
    SetForegroundWindow(s.hwnd);
    SetCapture(s.hwnd);

    MSG msg{};
    while (!s.done) {
        const BOOL ret = GetMessageW(&msg, nullptr, 0, 0);
        if (ret == -1) {
            s.done = true;
            break;
        }
        if (ret == 0) {
            PostQuitMessage(static_cast<int>(msg.wParam));
            s.done = true;
            break;
        }
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    if (s.hwnd) {
        ReleaseCapture();
        DestroyWindow(s.hwnd);
        s.hwnd = nullptr;
    }
    s.rt.Reset();
    s.theme = nullptr;

    return s.selected_id;
}

// ============================================================
// メッセージハンドラ
// ============================================================

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

        // ナビ行のボタン判定
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

// ============================================================
// メニュー項目構築
// ============================================================

void ContextMenu::Impl::BuildItems(const ContextMenuParams& params)
{
    items.clear();

    Item nav_row;
    nav_row.type = ItemType::NavRow;
    nav_row.id = 0;
    items.emplace_back(std::move(nav_row));
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

// ============================================================
// レイアウト計算
// ============================================================

void ContextMenu::Impl::ComputeLayout()
{
    float max_text_w = 0.0f;
    for (const auto& item : items) {
        if (item.type != ItemType::Text || item.text.empty()) {
            continue;
        }
        ComPtr<IDWriteTextLayout> layout;
        dwrite_factory->CreateTextLayout(
            item.text.data(), static_cast<UINT32>(item.text.size()),
            fmt_text.Get(), 1000.0f, 100.0f, &layout);
        if (layout) {
            DWRITE_TEXT_METRICS metrics{};
            layout->GetMetrics(&metrics);
            if (metrics.width > max_text_w) {
                max_text_w = metrics.width;
            }
        }
    }

    const float nav_row_w = 2 * NAV_BTN_SIZE + NAV_BTN_GAP + 2 * PAD_X;
    const float text_w = CHECK_WIDTH + max_text_w + PAD_X * 2;
    menu_width = (nav_row_w > text_w) ? nav_row_w : text_w;
    if (menu_width < 160.0f) {
        menu_width = 160.0f;
    }

    float y = PAD_Y;
    for (auto& item : items) {
        switch (item.type) {
        case ItemType::NavRow: {
            const float row_h = NAV_BTN_SIZE + 2 * NAV_ROW_PAD_Y;
            item.rect = { 0, y, menu_width, y + row_h };

            const float cx = menu_width / 2.0f;
            const float total_w = 2 * NAV_BTN_SIZE + NAV_BTN_GAP;
            const float bx = cx - total_w / 2.0f;
            const float by = y + NAV_ROW_PAD_Y;
            nav_layout.back_rect = { bx, by, bx + NAV_BTN_SIZE, by + NAV_BTN_SIZE };
            nav_layout.fwd_rect = { bx + NAV_BTN_SIZE + NAV_BTN_GAP, by, bx + total_w, by + NAV_BTN_SIZE };
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
        }
    }

    menu_height = y + PAD_Y;
}

// ============================================================
// D2Dリソース
// ============================================================

bool ContextMenu::Impl::EnsureRenderTarget(float dpi)
{
    if (rt) {
        return true;
    }
    RECT rc;
    GetClientRect(hwnd, &rc);
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
        rt->CreateSolidColorBrush(c, &b);
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

void ContextMenu::Impl::CreateTextFormats(const Theme& t)
{
    fmt_text.Reset();
    fmt_icon.Reset();

    dwrite_factory->CreateTextFormat(
        t.font_family.c_str(), nullptr,
        DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL,
        DWRITE_FONT_STRETCH_NORMAL, t.pane_font_size,
        L"ja-jp", &fmt_text);
    if (fmt_text) {
        fmt_text->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        fmt_text->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    }

    dwrite_factory->CreateTextFormat(
        L"Segoe Fluent Icons", nullptr,
        DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL,
        DWRITE_FONT_STRETCH_NORMAL, 14.0f,
        L"ja-jp", &fmt_icon);
    if (fmt_icon) {
        fmt_icon->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
        fmt_icon->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        fmt_icon->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    }
}

// ============================================================
// 描画
// ============================================================

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
        1.0f
    );
}

void ContextMenu::Impl::DrawTextItem(const Item& item)
{
    const bool hovered = (item.id != 0 && item.id == hovered_id && item.enabled);

    if (hovered) {
        const float margin = 4.0f;
        const D2D1_ROUNDED_RECT rr{ {
            item.rect.left + margin,
            item.rect.top + 1.0f,
            item.rect.right - margin,
            item.rect.bottom - 1.0f
        }, 4.0f, 4.0f };
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

// ============================================================
// ヒットテスト
// ============================================================

int ContextMenu::Impl::HitTest(float x, float y) const noexcept
{
    for (const auto& item : items) {
        if (item.type != ItemType::Text || item.id == 0) {
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
