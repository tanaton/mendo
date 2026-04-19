#include "context_menu.h"
#include "i18n.h"
#include "ui_constants.h"
#include <cmath>

using Microsoft::WRL::ComPtr;

bool ContextMenu::class_registered_ = false;

// ============================================================
// ウィンドウクラス登録
// ============================================================

bool ContextMenu::RegisterWindowClass()
{
    if (class_registered_) {
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
    class_registered_ = true;
    return true;
}

LRESULT CALLBACK ContextMenu::WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    ContextMenu* self = nullptr;
    if (msg == WM_NCCREATE) {
        const auto* cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
        self = static_cast<ContextMenu*>(cs->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        self->hwnd_ = hwnd;
    }
    else {
        self = reinterpret_cast<ContextMenu*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }
    if (self) {
        return self->HandleMessage(msg, wParam, lParam);
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

// ============================================================
// 初期化・破棄
// ============================================================

void ContextMenu::Init(ID2D1Factory* d2d_factory, IDWriteFactory* dwrite_factory)
{
    d2d_factory_ = d2d_factory;
    dwrite_factory_ = dwrite_factory;
}

ContextMenu::~ContextMenu()
{
    if (hwnd_) {
        DestroyWindow(hwnd_);
        hwnd_ = nullptr;
    }
}

// ============================================================
// メニュー表示（モーダル）
// ============================================================

int ContextMenu::Show(HWND owner, const ContextMenuParams& params)
{
    if (!d2d_factory_ || !dwrite_factory_ || !params.theme || params.dpi_scale <= 0.0f) {
        return 0;
    }

    owner_ = owner;
    theme_ = params.theme;
    dpi_scale_ = params.dpi_scale;

    BuildItems(params);
    CreateTextFormats(*theme_);
    ComputeLayout();

    // ウィンドウ作成（毎回作成・破棄で状態をクリーンに保つ）
    if (!RegisterWindowClass()) {
        return 0;
    }
    if (hwnd_) {
        DestroyWindow(hwnd_);
        hwnd_ = nullptr;
    }
    rt_.Reset();

    const int pixel_w = static_cast<int>(std::ceil(menu_width_ * dpi_scale_));
    const int pixel_h = static_cast<int>(std::ceil(menu_height_ * dpi_scale_));

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

    hwnd_ = CreateWindowExW(
        WS_EX_TOOLWINDOW | WS_EX_TOPMOST,
        L"mendoContextMenu", nullptr,
        WS_POPUP,
        x, y, pixel_w, pixel_h,
        owner, nullptr, GetModuleHandleW(nullptr), this);

    if (!hwnd_) {
        return 0;
    }

    const float dpi = dpi_scale_ * 96.0f;
    if (!EnsureRenderTarget(dpi)) {
        DestroyWindow(hwnd_);
        hwnd_ = nullptr;
        return 0;
    }
    CreateBrushes();

    // 角丸クリッピング用リージョン
    const int corner_px = static_cast<int>(MENU_CORNER * dpi_scale_);
    const HRGN rgn = CreateRoundRectRgn(0, 0, pixel_w + 1, pixel_h + 1, corner_px, corner_px);
    SetWindowRgn(hwnd_, rgn, FALSE);

    selected_id_ = 0;
    done_ = false;
    hovered_id_ = 0;
    hovered_nav_ = 0;

    ShowWindow(hwnd_, SW_SHOW);
    SetForegroundWindow(hwnd_);
    SetCapture(hwnd_);

    // モーダルメッセージループ
    MSG msg{};
    while (!done_) {
        const BOOL ret = GetMessageW(&msg, nullptr, 0, 0);
        if (ret == -1) {
            done_ = true;
            break;
        }
        if (ret == 0) {
            // WM_QUITを再ポストしてメインループに伝搬させる
            PostQuitMessage(static_cast<int>(msg.wParam));
            done_ = true;
            break;
        }
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    if (hwnd_) {
        ReleaseCapture();
        DestroyWindow(hwnd_);
        hwnd_ = nullptr;
    }
    rt_.Reset();
    theme_ = nullptr;

    return selected_id_;
}

// ============================================================
// メッセージハンドラ
// ============================================================

LRESULT ContextMenu::HandleMessage(UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg) {
    case WM_PAINT: {
        PAINTSTRUCT ps;
        BeginPaint(hwnd_, &ps);
        Paint();
        EndPaint(hwnd_, &ps);
        return 0;
    }

    case WM_MOUSEMOVE: {
        const float x = static_cast<short>(LOWORD(lParam)) / dpi_scale_;
        const float y = static_cast<short>(HIWORD(lParam)) / dpi_scale_;

        const int old_hovered = hovered_id_;
        const int old_nav = hovered_nav_;

        hovered_id_ = 0;
        hovered_nav_ = 0;

        // ナビ行のボタン判定
        if (!items_.empty() && items_[0].type == ItemType::NavRow) {
            if (nav_layout_.back_enabled && PointInRect(x, y, nav_layout_.back_rect)) {
                hovered_nav_ = -1;
            }
            else if (nav_layout_.fwd_enabled && PointInRect(x, y, nav_layout_.fwd_rect)) {
                hovered_nav_ = 1;
            }
        }

        if (hovered_nav_ == 0) {
            hovered_id_ = HitTest(x, y);
        }

        if (hovered_id_ != old_hovered || hovered_nav_ != old_nav) {
            InvalidateRect(hwnd_, nullptr, FALSE);
        }
        return 0;
    }

    case WM_LBUTTONDOWN:
    case WM_RBUTTONDOWN: {
        const float x = static_cast<short>(LOWORD(lParam)) / dpi_scale_;
        const float y = static_cast<short>(HIWORD(lParam)) / dpi_scale_;

        // メニュー外クリック → 閉じる
        if (x < 0 || y < 0 || x >= menu_width_ || y >= menu_height_) {
            done_ = true;
            return 0;
        }

        // ナビボタン判定（クリック座標から直接判定、キャッシュされたホバー状態に依存しない）
        const int nav_hit = NavHitTest(x, y);
        if (nav_hit != 0) {
            selected_id_ = nav_hit;
            done_ = true;
            return 0;
        }

        // テキスト項目判定
        const int hit = HitTest(x, y);
        for (const auto& item : items_) {
            if (item.id == hit && item.enabled) {
                selected_id_ = hit;
                done_ = true;
                return 0;
            }
        }
        return 0;
    }

    case WM_KEYDOWN:
        if (wParam == VK_ESCAPE) {
            done_ = true;
        }
        return 0;

    case WM_CAPTURECHANGED:
        // キャプチャを失った → 閉じる
        if (reinterpret_cast<HWND>(lParam) != hwnd_) {
            done_ = true;
        }
        return 0;

    case WM_KILLFOCUS:
        done_ = true;
        return 0;

    case WM_ACTIVATEAPP:
        if (wParam == FALSE) {
            done_ = true;
        }
        return 0;

    case WM_DESTROY:
        return 0;

    default:
        return DefWindowProcW(hwnd_, msg, wParam, lParam);
    }
}

// ============================================================
// メニュー項目構築
// ============================================================

void ContextMenu::BuildItems(const ContextMenuParams& params)
{
    items_.clear();

    // ナビゲーション行（戻る/進む横並び）
    Item nav_row;
    nav_row.type = ItemType::NavRow;
    nav_row.id = 0;
    items_.emplace_back(std::move(nav_row));
    nav_layout_.back_enabled = params.can_go_back;
    nav_layout_.fwd_enabled = params.can_go_forward;

    items_.emplace_back(ItemType::Separator);

    const auto& ls = i18n::S();
    if (params.show_file_items) {
        items_.emplace_back(ItemType::Text, IDM_EDIT_FILE, ls.menu_edit_file, params.has_file, false);
        items_.emplace_back(ItemType::Text, IDM_COPY, ls.menu_copy, params.has_selection, false);
        items_.emplace_back(ItemType::Text, IDM_COPY_FORMATTED, ls.menu_copy_formatted, params.has_selection, false);
        items_.emplace_back(ItemType::Separator);
    }
    items_.emplace_back(ItemType::Text, IDM_TOGGLE_DARK_MODE, ls.menu_dark_mode, true, params.dark_mode_checked);
    items_.emplace_back(ItemType::Separator);
    items_.emplace_back(ItemType::Text, IDM_TOGGLE_FILE_PANE, ls.menu_file_pane, true, params.file_pane_checked);
    items_.emplace_back(ItemType::Text, IDM_TOGGLE_TOC_PANE, ls.menu_toc_pane, true, params.toc_pane_checked);
}

// ============================================================
// レイアウト計算
// ============================================================

void ContextMenu::ComputeLayout()
{
    // テキスト項目の最大幅を測定
    float max_text_w = 0.0f;
    for (const auto& item : items_) {
        if (item.type != ItemType::Text || item.text.empty()) {
            continue;
        }
        ComPtr<IDWriteTextLayout> layout;
        dwrite_factory_->CreateTextLayout(
            item.text.data(), static_cast<UINT32>(item.text.size()),
            fmt_text_.Get(), 1000.0f, 100.0f, &layout);
        if (layout) {
            DWRITE_TEXT_METRICS metrics{};
            layout->GetMetrics(&metrics);
            if (metrics.width > max_text_w) {
                max_text_w = metrics.width;
            }
        }
    }

    // メニュー幅: テキスト幅 + パディング + チェック列 + 右マージン
    const float nav_row_w = 2 * NAV_BTN_SIZE + NAV_BTN_GAP + 2 * PAD_X;
    const float text_w = CHECK_WIDTH + max_text_w + PAD_X * 2;
    menu_width_ = (nav_row_w > text_w) ? nav_row_w : text_w;
    if (menu_width_ < 160.0f) {
        menu_width_ = 160.0f;
    }

    // 各項目のY座標を配置
    float y = PAD_Y;
    for (auto& item : items_) {
        switch (item.type) {
        case ItemType::NavRow: {
            const float row_h = NAV_BTN_SIZE + 2 * NAV_ROW_PAD_Y;
            item.rect = { 0, y, menu_width_, y + row_h };

            const float cx = menu_width_ / 2.0f;
            const float total_w = 2 * NAV_BTN_SIZE + NAV_BTN_GAP;
            const float bx = cx - total_w / 2.0f;
            const float by = y + NAV_ROW_PAD_Y;
            nav_layout_.back_rect = { bx, by, bx + NAV_BTN_SIZE, by + NAV_BTN_SIZE };
            nav_layout_.fwd_rect = { bx + NAV_BTN_SIZE + NAV_BTN_GAP, by, bx + total_w, by + NAV_BTN_SIZE };
            y += row_h;
            break;
        }
        case ItemType::Separator:
            item.rect = { 0, y, menu_width_, y + SEPARATOR_HEIGHT };
            y += SEPARATOR_HEIGHT;
            break;
        case ItemType::Text:
            item.rect = { 0, y, menu_width_, y + ITEM_HEIGHT };
            y += ITEM_HEIGHT;
            break;
        }
    }

    menu_height_ = y + PAD_Y;
}

// ============================================================
// D2Dリソース
// ============================================================

bool ContextMenu::EnsureRenderTarget(float dpi)
{
    if (rt_) {
        return true;
    }
    RECT rc;
    GetClientRect(hwnd_, &rc);
    const D2D1_SIZE_U size{ static_cast<UINT32>(rc.right), static_cast<UINT32>(rc.bottom) };
    D2D1_RENDER_TARGET_PROPERTIES rtProps = D2D1::RenderTargetProperties();
    rtProps.dpiX = dpi;
    rtProps.dpiY = dpi;
    const auto hwndProps = D2D1::HwndRenderTargetProperties(hwnd_, size);
    const HRESULT hr = d2d_factory_->CreateHwndRenderTarget(rtProps, hwndProps, &rt_);
    return SUCCEEDED(hr);
}

void ContextMenu::CreateBrushes()
{
    if (!rt_ || !theme_) {
        return;
    }
    auto make = [&](D2D1_COLOR_F c) {
        ComPtr<ID2D1SolidColorBrush> b;
        rt_->CreateSolidColorBrush(c, &b);
        return b;
    };

    brush_border_ = make(theme_->splitter_color);
    brush_text_ = make(theme_->text_color);

    auto gray = theme_->text_color;
    gray.a = 0.35f;
    brush_gray_ = make(gray);

    brush_hover_ = make(theme_->pane_item_hover_color);

    // チェックマーク色 = リンク色
    brush_check_ = make(theme_->link_color);
}

void ContextMenu::CreateTextFormats(const Theme& theme)
{
    fmt_text_.Reset();
    fmt_icon_.Reset();

    dwrite_factory_->CreateTextFormat(
        theme.font_family.c_str(), nullptr,
        DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL,
        DWRITE_FONT_STRETCH_NORMAL, theme.pane_font_size,
        L"ja-jp", &fmt_text_);
    if (fmt_text_) {
        fmt_text_->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        fmt_text_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    }

    dwrite_factory_->CreateTextFormat(
        L"Segoe Fluent Icons", nullptr,
        DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL,
        DWRITE_FONT_STRETCH_NORMAL, 14.0f,
        L"ja-jp", &fmt_icon_);
    if (fmt_icon_) {
        fmt_icon_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
        fmt_icon_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        fmt_icon_->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    }
}

// ============================================================
// 描画
// ============================================================

void ContextMenu::Paint()
{
    if (!rt_) {
        return;
    }
    rt_->BeginDraw();
    rt_->Clear(theme_->pane_bg_color);

    // 枠線
    const D2D1_RECT_F border_rect{
        MENU_BORDER * 0.5f,
        MENU_BORDER * 0.5f,
        menu_width_ - MENU_BORDER * 0.5f,
        menu_height_ - MENU_BORDER * 0.5f
    };
    const D2D1_ROUNDED_RECT rr{ border_rect, MENU_CORNER, MENU_CORNER };
    rt_->DrawRoundedRectangle(rr, brush_border_.Get(), MENU_BORDER);

    for (const auto& item : items_) {
        switch (item.type) {
        case ItemType::NavRow:
            DrawNavRow(item);
            break;
        case ItemType::Separator:
            DrawSeparator(item);
            break;
        case ItemType::Text:
            DrawTextItem(item);
            break;
        }
    }

    const HRESULT hr = rt_->EndDraw();
    if (hr == D2DERR_RECREATE_TARGET) {
        rt_.Reset();
    }
}

void ContextMenu::DrawNavRow(const Item& /*item*/)
{
    auto draw_btn = [&](const D2D1_RECT_F& rc, const wchar_t* glyph, bool enabled, bool hovered) {
        if (hovered) {
            const D2D1_ROUNDED_RECT rr = { rc, NAV_BTN_CORNER, NAV_BTN_CORNER };
            rt_->FillRoundedRectangle(rr, brush_hover_.Get());
        }
        auto* brush = enabled ? brush_text_.Get() : brush_gray_.Get();
        if (fmt_icon_) {
            rt_->DrawText(glyph, 1, fmt_icon_.Get(), rc, brush);
        }
    };

    draw_btn(nav_layout_.back_rect, GLYPH_BACK, nav_layout_.back_enabled, hovered_nav_ == -1);
    draw_btn(nav_layout_.fwd_rect, GLYPH_FORWARD, nav_layout_.fwd_enabled, hovered_nav_ == 1);
}

void ContextMenu::DrawSeparator(const Item& item)
{
    const float cy = (item.rect.top + item.rect.bottom) / 2.0f;
    const float margin = 12.0f;
    rt_->DrawLine(
        { item.rect.left + margin, cy },
        { item.rect.right - margin, cy },
        brush_border_.Get(),
        1.0f
    );
}

void ContextMenu::DrawTextItem(const Item& item)
{
    const bool hovered = (item.id != 0 && item.id == hovered_id_ && item.enabled);

    if (hovered) {
        const float margin = 4.0f;
        const D2D1_ROUNDED_RECT rr{ {
            item.rect.left + margin,
            item.rect.top + 1.0f,
            item.rect.right - margin,
            item.rect.bottom - 1.0f
        }, 4.0f, 4.0f };
        rt_->FillRoundedRectangle(rr, brush_hover_.Get());
    }

    auto* brush = item.enabled ? brush_text_.Get() : brush_gray_.Get();

    // チェックマーク
    if (item.checked) {
        const D2D1_RECT_F check_rc = {
            item.rect.left + 8.0f, item.rect.top,
            item.rect.left + CHECK_WIDTH + 4.0f, item.rect.bottom
        };
        if (fmt_icon_) {
            rt_->DrawText(GLYPH_CHECKMARK, 1, fmt_icon_.Get(), check_rc, brush_check_.Get());
        }
    }

    // テキスト（fmt_text_にPARAGRAPH_ALIGNMENT_CENTERが設定済み）
    if (fmt_text_ && !item.text.empty()) {
        const D2D1_RECT_F text_rc = {
            item.rect.left + PAD_X, item.rect.top,
            item.rect.right - 8.0f, item.rect.bottom
        };
        rt_->DrawText(item.text.data(), static_cast<UINT32>(item.text.size()), fmt_text_.Get(), text_rc, brush);
    }
}

// ============================================================
// ヒットテスト
// ============================================================

int ContextMenu::HitTest(float x, float y) const noexcept
{
    for (const auto& item : items_) {
        if (item.type != ItemType::Text || item.id == 0) {
            continue;
        }
        if (PointInRect(x, y, item.rect)) {
            return item.id;
        }
    }
    return 0;
}

int ContextMenu::NavHitTest(float x, float y) const noexcept
{
    if (nav_layout_.back_enabled && PointInRect(x, y, nav_layout_.back_rect)) {
        return IDM_NAV_BACK;
    }
    if (nav_layout_.fwd_enabled && PointInRect(x, y, nav_layout_.fwd_rect)) {
        return IDM_NAV_FORWARD;
    }
    return 0;
}
