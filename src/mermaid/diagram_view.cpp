#include "diagram_view.h"
#include "app_constants.h"
#include "d2d_util.h"
#include "diagram_raster.h"
#include "i18n.h"
#include "ui_constants.h"
#include <d2d1_3.h>
#include <windowsx.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <format>
#include <string>

using Microsoft::WRL::ComPtr;

namespace {

// mermaid_viewer.html の #bar と同じ寸法 (DIP)。窓の右端からの位置は、スクロールバーがクライアント領域の外にあるので上と同じにする。
constexpr float kBarMargin = 12.0f;
constexpr float kBarPadding = 4.0f;
constexpr float kBarGap = 2.0f;
constexpr float kButtonWidth = 30.0f;
constexpr float kButtonHeight = 28.0f;
constexpr float kLabelWidth = 48.0f;
constexpr float kBarCorner = 6.0f;
constexpr float kButtonCorner = 4.0f;
// 矢印キーとスクロールバーの矢印の 1 回分。ホイールは 1 行をこの長さとして SPI_GETWHEELSCROLLLINES 行。
constexpr float kLineStep = 40.0f;
// ページ単位の送りは窓の 9 割にして、前の画面の端を残す。
constexpr float kPageRatio = 0.9f;
// 端まで送る。ScrollTo が範囲に収める。
constexpr float kFar = 1.0e9f;

struct Palette {
    D2D1_COLOR_F background;
    D2D1_COLOR_F text;
    D2D1_COLOR_F bar;
    D2D1_COLOR_F hover;
    D2D1_COLOR_F shadow;
};

// mermaid_viewer.html の色と同じ。背景は図の背景 (DrawDiagram) とそろえる。
Palette PaletteFor(bool dark) noexcept
{
    if (dark) {
        return { D2D1::ColorF(0x1E1E1E), D2D1::ColorF(0xDDDDDD), D2D1::ColorF(0x323232, 0.94f), D2D1::ColorF(0xFFFFFF, 0.15f),
                 D2D1::ColorF(0x000000, 0.5f) };
    }
    return { D2D1::ColorF(0xFFFFFF), D2D1::ColorF(0x333333), D2D1::ColorF(0xF0F0F0, 0.94f), D2D1::ColorF(0x000000, 0.1f),
             D2D1::ColorF(0x000000, 0.18f) };
}

} // namespace

float diagram_view::FitZoom(float view_width, float view_height, float natural_width, float natural_height) noexcept
{
    if (!(natural_width > 0.0f && natural_height > 0.0f)) {
        return 1.0f;
    }
    return std::max(kZoomStops[0], std::min({ 1.0f, (view_width - kPadding * 2.0f) / natural_width, (view_height - kPadding * 2.0f) / natural_height }));
}

std::optional<float> diagram_view::NextZoom(float zoom, float fit_zoom, int dir) noexcept
{
    std::array<float, std::size(kZoomStops) + 1> stops{};
    size_t count = 0;
    for (const float s : kZoomStops) {
        if (std::abs(s / fit_zoom - 1.0f) > 0.1f) {
            stops[count++] = s;
        }
    }
    stops[count++] = fit_zoom;
    const auto used = std::span{ stops }.first(count);
    std::ranges::sort(used);
    if (dir > 0) {
        const auto it = std::ranges::upper_bound(used, zoom * 1.001f);
        return it == used.end() ? std::nullopt : std::optional{ *it };
    }
    const auto it = std::ranges::lower_bound(used, zoom * 0.999f);
    return it == used.begin() ? std::nullopt : std::optional{ *std::prev(it) };
}

DiagramView::DiagramView(HWND hwnd, IDWriteFactory* dw, IWICImagingFactory* wic) : hwnd_(hwnd), dw_(dw), wic_(wic)
{
    D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, d2d_.GetAddressOf());
    tooltip_.Init(hwnd);
}

// 窓は残して WebView2 の表示に替えることがあるので、出したスクロールバーを片付ける。
DiagramView::~DiagramView()
{
    KillTimer(hwnd_, kTooltipTimer);
    ShowScrollBar(hwnd_, SB_BOTH, FALSE);
}

void DiagramView::ApplyLanguage()
{
    // mermaid_viewer.html と同じく、倍率と読み込み中は 13px、ボタンの記号は 16px。
    const auto make = [&](float size) {
        return CreateTextFormat(dw_.Get(), { .family = L"Segoe UI",
                                                       .size = size,
                                                       .locale = i18n::S().lang_locale,
                                                       .text_align = DWRITE_TEXT_ALIGNMENT_CENTER,
                                                       .para_align = DWRITE_PARAGRAPH_ALIGNMENT_CENTER,
                                                       .no_wrap = true });
    };
    fmt_label_ = make(13.0f);
    fmt_symbol_ = make(16.0f);
    SetHover(Button::None);
    Invalidate();
}

void DiagramView::SetDarkMode(bool dark)
{
    dark_ = dark;
    tooltip_.ApplyDarkMode(dark);
    Invalidate();
}

void DiagramView::ShowLoading()
{
    drawing_.reset();
    commands_.Reset();
    SetHover(Button::None);
    Layout();
}

void DiagramView::SetDrawing(std::shared_ptr<const mermaid_util::DiagramDrawing> drawing)
{
    const bool keep = drawing_ != nullptr;
    drawing_ = std::move(drawing);
    commands_.Reset();
    if (keep) {
        Layout();
    }
    else {
        Fit();
    }
}

float DiagramView::Scale() const noexcept
{
    return static_cast<float>(GetDpiForWindow(hwnd_)) / USER_DEFAULT_SCREEN_DPI;
}

D2D1_POINT_2F DiagramView::ToDip(LPARAM lparam) const noexcept
{
    const float scale = Scale();
    return { static_cast<float>(GET_X_LPARAM(lparam)) / scale, static_cast<float>(GET_Y_LPARAM(lparam)) / scale };
}

bool DiagramView::EnsureTarget()
{
    if (rt_) {
        return true;
    }
    RECT rc{};
    GetClientRect(hwnd_, &rc);
    const float dpi = Scale() * USER_DEFAULT_SCREEN_DPI;
    // D2D1_PRESENT_OPTIONS_IMMEDIATELY は、スクロールバーが出て窓が縮んだ直後の 1 枚が画面に出ないことがあった (2 回に 1 回)。
    if (!d2d_ || FAILED(d2d_->CreateHwndRenderTarget(D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_DEFAULT, D2D1::PixelFormat(), dpi, dpi),
                                                     D2D1::HwndRenderTargetProperties(hwnd_, D2D1::SizeU(rc.right, rc.bottom)),
                                                     &rt_)) ||
        FAILED(rt_->CreateSolidColorBrush(D2D1::ColorF(0), &brush_))) {
        rt_.Reset();
        return false;
    }
    record_dc_ = CreateRecordContext(rt_.Get());
    return true;
}

D2D1_POINT_2F DiagramView::Origin() const noexcept
{
    const auto place = [](float content, float view, float scroll) {
        return content + diagram_view::kPadding * 2.0f <= view ? (view - content) / 2.0f : diagram_view::kPadding - scroll;
    };
    return { place(drawing_->size.width * zoom_, view_width_, scroll_x_), place(drawing_->size.height * zoom_, view_height_, scroll_y_) };
}

void DiagramView::Paint()
{
    PAINTSTRUCT ps;
    BeginPaint(hwnd_, &ps);
    EndPaint(hwnd_, &ps);
    if (!EnsureTarget()) {
        return;
    }
    const Palette palette = PaletteFor(dark_);
    rt_->BeginDraw();
    rt_->SetTransform(D2D1::Matrix3x2F::Identity());
    rt_->Clear(palette.background);
    if (drawing_) {
        if (!commands_) {
            commands_ = RecordDiagram(record_dc_.Get(), dw_.Get(), wic_.Get(), *drawing_, i18n::TextLocale());
        }
        ComPtr<ID2D1DeviceContext> dc;
        if (commands_ && SUCCEEDED(rt_.As(&dc))) {
            const D2D1_POINT_2F origin = Origin();
            dc->SetTransform(D2D1::Matrix3x2F::Scale(zoom_, zoom_) * D2D1::Matrix3x2F::Translation(origin.x, origin.y));
            dc->DrawImage(commands_.Get());
            dc->SetTransform(D2D1::Matrix3x2F::Identity());
        }
        PaintBar(brush_.Get());
    }
    else if (fmt_label_) {
        const std::wstring_view loading = i18n::S().loading;
        brush_->SetColor(palette.text);
        rt_->DrawTextW(loading.data(), static_cast<UINT32>(loading.size()), fmt_label_.Get(), D2D1::RectF(0.0f, 0.0f, view_width_, view_height_),
                       brush_.Get());
    }
    if (rt_->EndDraw() == D2DERR_RECREATE_TARGET) {
        rt_.Reset();
        brush_.Reset();
        record_dc_.Reset();
        commands_.Reset();
        Invalidate();
    }
}

D2D1_RECT_F DiagramView::BarRect() const noexcept
{
    const float width = kBarPadding * 2.0f + kButtonWidth * 3.0f + kLabelWidth + kBarGap * 3.0f;
    const float right = view_width_ - kBarMargin;
    return D2D1::RectF(right - width, kBarMargin, right, kBarMargin + kBarPadding * 2.0f + kButtonHeight);
}

D2D1_RECT_F DiagramView::ButtonRect(Button button) const noexcept
{
    const D2D1_RECT_F bar = BarRect();
    float left = bar.left + kBarPadding;
    switch (button) {
    case Button::ZoomOut:
        break;
    case Button::ZoomIn:
        left += kButtonWidth + kLabelWidth + kBarGap * 2.0f;
        break;
    case Button::Fit:
        left += kButtonWidth * 2.0f + kLabelWidth + kBarGap * 3.0f;
        break;
    case Button::None:
        return {};
    }
    return D2D1::RectF(left, bar.top + kBarPadding, left + kButtonWidth, bar.top + kBarPadding + kButtonHeight);
}

bool DiagramView::IsEnabled(Button button) const noexcept
{
    switch (button) {
    case Button::ZoomIn:
        return diagram_view::NextZoom(zoom_, fit_zoom_, 1).has_value();
    case Button::ZoomOut:
        return diagram_view::NextZoom(zoom_, fit_zoom_, -1).has_value();
    case Button::Fit:
        return true;
    case Button::None:
        break;
    }
    return false;
}

DiagramView::Button DiagramView::HitButton(float x, float y) const noexcept
{
    if (!drawing_) {
        return Button::None;
    }
    for (const Button b : { Button::ZoomOut, Button::ZoomIn, Button::Fit }) {
        if (PointInRect(x, y, ButtonRect(b))) {
            return b;
        }
    }
    return Button::None;
}

void DiagramView::PaintBar(ID2D1SolidColorBrush* brush)
{
    const Palette palette = PaletteFor(dark_);
    const D2D1_RECT_F bar = BarRect();
    brush->SetColor(palette.shadow);
    rt_->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(bar.left, bar.top + 1.0f, bar.right, bar.bottom + 1.5f), kBarCorner, kBarCorner), brush);
    brush->SetColor(palette.bar);
    rt_->FillRoundedRectangle(D2D1::RoundedRect(bar, kBarCorner, kBarCorner), brush);
    for (const Button b : { Button::ZoomOut, Button::ZoomIn, Button::Fit }) {
        const D2D1_RECT_F r = ButtonRect(b);
        const bool enabled = IsEnabled(b);
        if ((hover_ == b || pressed_ == b) && enabled) {
            brush->SetColor(palette.hover);
            rt_->FillRoundedRectangle(D2D1::RoundedRect(r, kButtonCorner, kButtonCorner), brush);
        }
        D2D1_COLOR_F glyph = palette.text;
        glyph.a = enabled ? 1.0f : DISABLED_UI_ALPHA;
        brush->SetColor(glyph);
        if (b == Button::Fit) {
            // mermaid_viewer.html の全体表示のアイコン (16x16 の四隅の鉤)。
            const DipPoint c = RectCenter(r);
            const float ox = c.x - 8.0f;
            const float oy = c.y - 8.0f;
            constexpr float kCorners[4][6]{
                { 1.5f, 5.5f, 1.5f, 1.5f, 5.5f, 1.5f },
                { 10.5f, 1.5f, 14.5f, 1.5f, 14.5f, 5.5f },
                { 14.5f, 10.5f, 14.5f, 14.5f, 10.5f, 14.5f },
                { 5.5f, 14.5f, 1.5f, 14.5f, 1.5f, 10.5f },
            };
            for (const auto& k : kCorners) {
                rt_->DrawLine(D2D1::Point2F(ox + k[0], oy + k[1]), D2D1::Point2F(ox + k[2], oy + k[3]), brush, 1.5f);
                rt_->DrawLine(D2D1::Point2F(ox + k[2], oy + k[3]), D2D1::Point2F(ox + k[4], oy + k[5]), brush, 1.5f);
            }
        }
        else if (fmt_symbol_) {
            const wchar_t* symbol = b == Button::ZoomIn ? L"+" : L"\x2212";
            rt_->DrawTextW(symbol, 1, fmt_symbol_.Get(), r, brush);
        }
    }
    if (fmt_label_) {
        const D2D1_RECT_F out = ButtonRect(Button::ZoomOut);
        const std::wstring zoom = std::format(L"{}%", std::lround(zoom_ * 100.0f));
        brush->SetColor(palette.text);
        rt_->DrawTextW(zoom.data(), static_cast<UINT32>(zoom.size()), fmt_label_.Get(),
                       D2D1::RectF(out.right + kBarGap, out.top, out.right + kBarGap + kLabelWidth, out.bottom), brush);
    }
}

void DiagramView::Layout()
{
    if (laying_out_) {
        return;
    }
    laying_out_ = true;
    // スクロールバーを出し入れするとクライアント領域が変わるので、変わらなくなるまで決め直す (縦横で 2 回まで)。
    for (int i = 0; i < 3; ++i) {
        RECT rc{};
        GetClientRect(hwnd_, &rc);
        const float scale = Scale();
        view_width_ = static_cast<float>(rc.right) / scale;
        view_height_ = static_cast<float>(rc.bottom) / scale;
        if (rt_) {
            rt_->SetDpi(scale * USER_DEFAULT_SCREEN_DPI, scale * USER_DEFAULT_SCREEN_DPI);
            rt_->Resize(D2D1::SizeU(rc.right, rc.bottom));
        }
        if (drawing_) {
            fit_zoom_ = diagram_view::FitZoom(view_width_, view_height_, drawing_->size.width, drawing_->size.height);
            if (fitting_) {
                zoom_ = fit_zoom_;
            }
        }
        UpdateScrollBars();
        RECT after{};
        GetClientRect(hwnd_, &after);
        if (after.right == rc.right && after.bottom == rc.bottom) {
            break;
        }
    }
    laying_out_ = false;
    Invalidate();
}

void DiagramView::UpdateScrollBars()
{
    const float scale = Scale();
    const auto apply = [&](int bar, float content, float view, float& scroll) {
        const float max_scroll = std::max(0.0f, content - view);
        scroll = std::clamp(scroll, 0.0f, max_scroll);
        SCROLLINFO si{ .cbSize = sizeof(si), .fMask = SIF_RANGE | SIF_PAGE | SIF_POS };
        if (max_scroll > 0.0f) {
            si.nMax = static_cast<int>(std::ceil(content * scale)) - 1;
            si.nPage = static_cast<UINT>(std::floor(view * scale));
            si.nPos = static_cast<int>(std::lround(scroll * scale));
        }
        SetScrollInfo(hwnd_, bar, &si, TRUE);
    };
    const float pad = diagram_view::kPadding * 2.0f;
    apply(SB_HORZ, drawing_ ? drawing_->size.width * zoom_ + pad : 0.0f, view_width_, scroll_x_);
    apply(SB_VERT, drawing_ ? drawing_->size.height * zoom_ + pad : 0.0f, view_height_, scroll_y_);
}

void DiagramView::ScrollTo(float x, float y)
{
    if (!drawing_) {
        return;
    }
    const float pad = diagram_view::kPadding * 2.0f;
    x = std::clamp(x, 0.0f, std::max(0.0f, drawing_->size.width * zoom_ + pad - view_width_));
    y = std::clamp(y, 0.0f, std::max(0.0f, drawing_->size.height * zoom_ + pad - view_height_));
    // 端を越えて送っても描き直さない。
    if (x == scroll_x_ && y == scroll_y_) {
        return;
    }
    scroll_x_ = x;
    scroll_y_ = y;
    UpdateScrollBars();
    Invalidate();
}

void DiagramView::Scroll(int bar, WORD request)
{
    const bool horizontal = bar == SB_HORZ;
    const float page = (horizontal ? view_width_ : view_height_) * kPageRatio;
    float delta = 0.0f;
    switch (request) {
    case SB_LINEUP:
        delta = -kLineStep;
        break;
    case SB_LINEDOWN:
        delta = kLineStep;
        break;
    case SB_PAGEUP:
        delta = -page;
        break;
    case SB_PAGEDOWN:
        delta = page;
        break;
    case SB_TOP:
        delta = -kFar;
        break;
    case SB_BOTTOM:
        delta = kFar;
        break;
    case SB_THUMBTRACK:
    case SB_THUMBPOSITION: {
        SCROLLINFO si{ .cbSize = sizeof(si), .fMask = SIF_TRACKPOS };
        GetScrollInfo(hwnd_, bar, &si);
        delta = static_cast<float>(si.nTrackPos) / Scale() - (horizontal ? scroll_x_ : scroll_y_);
        break;
    }
    default:
        return;
    }
    ScrollBy(horizontal ? delta : 0.0f, horizontal ? 0.0f : delta);
}

void DiagramView::SetZoom(float zoom)
{
    const D2D1_POINT_2F before = Origin();
    // 窓の中央にある図の上の点 (倍率 1 の座標)。
    const float ax = (view_width_ / 2.0f - before.x) / zoom_;
    const float ay = (view_height_ / 2.0f - before.y) / zoom_;
    zoom_ = zoom;
    Layout();
    // 新しい倍率で同じ点が中央に来るスクロール位置。余白込みの面では図の左上が kPadding にある。
    ScrollTo(diagram_view::kPadding + ax * zoom_ - view_width_ / 2.0f, diagram_view::kPadding + ay * zoom_ - view_height_ / 2.0f);
}

void DiagramView::Fit()
{
    fitting_ = true;
    scroll_x_ = 0.0f;
    scroll_y_ = 0.0f;
    Layout();
}

void DiagramView::Step(int dir)
{
    if (const auto next = drawing_ ? diagram_view::NextZoom(zoom_, fit_zoom_, dir) : std::nullopt) {
        fitting_ = false;
        SetZoom(*next);
    }
}

void DiagramView::SetHover(Button button)
{
    if (hover_ == button) {
        return;
    }
    hover_ = button;
    const auto& s = i18n::S();
    tooltip_.SetText(button == Button::ZoomIn    ? s.viewer_zoom_in
                     : button == Button::ZoomOut ? s.viewer_zoom_out
                     : button == Button::Fit     ? s.viewer_fit
                                                 : std::wstring_view{});
    if (button == Button::None) {
        KillTimer(hwnd_, kTooltipTimer);
    }
    else {
        SetTimer(hwnd_, kTooltipTimer, app_timer::TOOLTIP_DELAY_MS, nullptr);
    }
    Invalidate();
}

bool DiagramView::HandleMessage(UINT msg, WPARAM wparam, LPARAM lparam, LRESULT& result)
{
    result = 0;
    switch (msg) {
    case WM_PAINT:
        Paint();
        return true;

    case WM_ERASEBKGND:
        result = 1;
        return true;

    case WM_SIZE:
        if (wparam != SIZE_MINIMIZED) {
            Layout();
        }
        return true;

    case WM_TIMER:
        if (wparam != kTooltipTimer) {
            return false;
        }
        KillTimer(hwnd_, kTooltipTimer);
        if (hover_ != Button::None) {
            tooltip_.Show();
        }
        return true;

    case WM_HSCROLL:
    case WM_VSCROLL:
        Scroll(msg == WM_HSCROLL ? SB_HORZ : SB_VERT, LOWORD(wparam));
        return true;

    case WM_MOUSEWHEEL:
    case WM_MOUSEHWHEEL: {
        // ホイールはスクロール (GitHub と同じ)。Shift で横。倍率はボタンとキーで変える。
        UINT lines = 3;
        SystemParametersInfoW(SPI_GETWHEELSCROLLLINES, 0, &lines, 0);
        const float notches = static_cast<float>(GET_WHEEL_DELTA_WPARAM(wparam)) / WHEEL_DELTA;
        const bool horizontal = msg == WM_MOUSEHWHEEL || (GET_KEYSTATE_WPARAM(wparam) & MK_SHIFT);
        const float step = lines == WHEEL_PAGESCROLL ? (horizontal ? view_width_ : view_height_) * kPageRatio : kLineStep * static_cast<float>(lines);
        // 縦のホイールは前へ回すと上へ、横のホイールは右へ倒すと右へ。
        const float amount = (msg == WM_MOUSEHWHEEL ? notches : -notches) * step;
        ScrollBy(horizontal ? amount : 0.0f, horizontal ? 0.0f : amount);
        return true;
    }

    case WM_LBUTTONDOWN: {
        SetFocus(hwnd_);
        const auto p = ToDip(lparam);
        const Button b = HitButton(p.x, p.y);
        if (b != Button::None) {
            if (IsEnabled(b)) {
                pressed_ = b;
                SetCapture(hwnd_);
                Invalidate();
            }
        }
        else if (drawing_) {
            drag_anchor_ = D2D1::Point2F(p.x + scroll_x_, p.y + scroll_y_);
            SetCapture(hwnd_);
            SetCursor(LoadCursorW(nullptr, IDC_SIZEALL));
        }
        tooltip_.SetText({});
        return true;
    }

    case WM_MOUSEMOVE: {
        const auto p = ToDip(lparam);
        if (!tracking_leave_) {
            TRACKMOUSEEVENT tme{ .cbSize = sizeof(tme), .dwFlags = TME_LEAVE, .hwndTrack = hwnd_ };
            tracking_leave_ = TrackMouseEvent(&tme) != FALSE;
        }
        if (drag_anchor_) {
            ScrollTo(drag_anchor_->x - p.x, drag_anchor_->y - p.y);
        }
        else if (pressed_ == Button::None) {
            SetHover(HitButton(p.x, p.y));
        }
        return true;
    }

    case WM_LBUTTONUP: {
        const auto p = ToDip(lparam);
        const Button pressed = std::exchange(pressed_, Button::None);
        drag_anchor_.reset();
        if (GetCapture() == hwnd_) {
            ReleaseCapture();
        }
        if (pressed != Button::None && HitButton(p.x, p.y) == pressed) {
            if (pressed == Button::Fit) {
                Fit();
            }
            else {
                Step(pressed == Button::ZoomIn ? 1 : -1);
            }
        }
        Invalidate();
        return true;
    }

    case WM_CAPTURECHANGED:
        drag_anchor_.reset();
        pressed_ = Button::None;
        Invalidate();
        return true;

    case WM_MOUSELEAVE:
        tracking_leave_ = false;
        SetHover(Button::None);
        return true;

    case WM_SETCURSOR:
        if (LOWORD(lparam) != HTCLIENT) {
            return false;
        }
        SetCursor(LoadCursorW(nullptr, drag_anchor_ ? IDC_SIZEALL : IDC_ARROW));
        result = TRUE;
        return true;

    case WM_KEYDOWN:
        switch (wparam) {
        case VK_ESCAPE:
            PostMessageW(hwnd_, WM_CLOSE, 0, 0);
            return true;
        case VK_LEFT:
            Scroll(SB_HORZ, SB_LINEUP);
            return true;
        case VK_RIGHT:
            Scroll(SB_HORZ, SB_LINEDOWN);
            return true;
        case VK_UP:
            Scroll(SB_VERT, SB_LINEUP);
            return true;
        case VK_DOWN:
            Scroll(SB_VERT, SB_LINEDOWN);
            return true;
        case VK_PRIOR:
            Scroll(SB_VERT, SB_PAGEUP);
            return true;
        case VK_NEXT:
        case VK_SPACE:
            Scroll(SB_VERT, SB_PAGEDOWN);
            return true;
        case VK_HOME:
            ScrollTo(0.0f, 0.0f);
            return true;
        case VK_END:
            Scroll(SB_VERT, SB_BOTTOM);
            return true;
        default:
            return false;
        }

    // キーの配置に依らず、打った文字で倍率を変える (mermaid_viewer.js の keydown の e.key と同じ)。
    case WM_CHAR:
        if (GetKeyState(VK_CONTROL) < 0 || GetKeyState(VK_MENU) < 0) {
            return false;
        }
        switch (wparam) {
        case L'+':
        case L'=':
            Step(1);
            return true;
        case L'-':
            Step(-1);
            return true;
        case L'0':
            if (drawing_) {
                Fit();
            }
            return true;
        default:
            return false;
        }

    default:
        return false;
    }
}
