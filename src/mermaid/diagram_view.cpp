#include "diagram_view.h"
#include "d2d_util.h"
#include "diagram_raster.h"
#include "i18n.h"
#include "pane_layout.h"
#include "resource.h"
#include "ui_constants.h"
#include "win32_util.h"
#include <d2d1_3.h>
#include <windowsx.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <format>
#include <string>

using Microsoft::WRL::ComPtr;

namespace {

// 8 倍は試作で確かめた範囲。
constexpr float kZoomStops[]{ 0.05f, 0.1f, 0.25f, 0.33f, 0.5f, 0.67f, 0.75f, 1.0f, 1.25f, 1.5f, 2.0f, 3.0f, 4.0f, 6.0f, 8.0f };
constexpr float kPadding = 16.0f;

constexpr std::wstring_view kWindowClass = L"mendo_MermaidViewer";
// 最初の大きさ。本体に対する割合。
constexpr int kSizePercent = 90;

// 右上のバーの寸法 (DIP)。窓の右端からの位置は、縦のスクロールバーが出ているときの位置に固定する。クライアント領域の
// 右端に付けると、拡大でスクロールバーが出た瞬間にバーが左へずれ、連打した + の位置に全体表示のボタンが来る (#408)。
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

Palette PaletteFor(bool dark) noexcept
{
    if (dark) {
        return { D2D1::ColorF(kDiagramDarkBackground), D2D1::ColorF(0xDDDDDD), D2D1::ColorF(0x323232, 0.94f), D2D1::ColorF(0xFFFFFF, 0.15f),
                 D2D1::ColorF(0x000000, 0.5f) };
    }
    return { D2D1::ColorF(kDiagramLightBackground), D2D1::ColorF(0x333333), D2D1::ColorF(0xF0F0F0, 0.94f), D2D1::ColorF(0x000000, 0.1f),
             D2D1::ColorF(0x000000, 0.18f) };
}

// 余白込みの面の大きさ。
float ContentExtent(float size, float zoom) noexcept
{
    return size * zoom + kPadding * 2.0f;
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

DiagramView::DiagramView(ID2D1Factory* d2d, IDWriteFactory* dw, IWICImagingFactory* wic, std::move_only_function<void()> on_close)
    : on_close_(std::move(on_close)), dw_(dw), wic_(wic), d2d_(d2d)
{}

std::unique_ptr<DiagramView> DiagramView::Open(HWND owner, ID2D1Factory* d2d, IDWriteFactory* dw, IWICImagingFactory* wic, bool dark,
                                               std::move_only_function<void()> on_close)
{
    static const bool registered = RegisterWindowClass(
        kWindowClass.data(), WndProc, LoadIconW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(IDI_APP_ICON)), LoadCursorW(nullptr, IDC_ARROW));
    RECT owner_rect{};
    if (!registered || !GetWindowRect(owner, &owner_rect)) {
        return nullptr;
    }
    std::unique_ptr<DiagramView> view(new DiagramView(d2d, dw, wic, std::move(on_close)));
    const int owner_w = owner_rect.right - owner_rect.left;
    const int owner_h = owner_rect.bottom - owner_rect.top;
    const int w = MulDiv(owner_w, kSizePercent, 100);
    const int h = MulDiv(owner_h, kSizePercent, 100);
    // 作る間のメッセージは HandleMessage に回さない (hwnd_ は作り終えてから入れる)。
    const HWND hwnd = CreateWindowExW(
        // 所有ウィンドウは既定でタスクバーにボタンが無く、最小化すると戻す手段が無い。図を見ながら編集するときにどかせるよう、
        // 最小化ボタンを消さずにタスクバーのボタンを出す。
        WS_EX_APPWINDOW,
        kWindowClass.data(),
        i18n::S().viewer_title.data(),
        WS_OVERLAPPEDWINDOW,
        owner_rect.left + (owner_w - w) / 2, owner_rect.top + (owner_h - h) / 2,
        w, h,
        owner, nullptr, GetModuleHandleW(nullptr), view.get());
    if (!hwnd) {
        return nullptr;
    }
    view->hwnd_ = hwnd;
    view->tooltip_.Init(hwnd, kTooltipTimer);
    // 表示する前に整え、最初の描画から背景を塗る。
    view->SetDarkMode(dark);
    view->ApplyLanguage();
    view->ShowLoading();
    // 表示の中の WM_WINDOWPOSCHANGED で解く。
    SetCloaked(hwnd, TRUE);
    ShowWindow(hwnd, SW_SHOW);
    return view;
}

DiagramView::~DiagramView()
{
    if (!hwnd_) {
        return;
    }
    // 窓を壊す間のメッセージは、壊しかけのこのオブジェクトに回さない (非表示の cloak は WndProc がオブジェクト無しで行う)。
    // ツールチップの窓はこの窓が所有するので一緒に壊れる。
    SetWindowLongPtrW(hwnd_, GWLP_USERDATA, 0);
    DestroyWindow(std::exchange(hwnd_, nullptr));
}

void DiagramView::BringToFront() const noexcept
{
    if (IsIconic(hwnd_)) {
        ShowWindow(hwnd_, SW_RESTORE);
    }
    SetForegroundWindow(hwnd_);
}

LRESULT CALLBACK DiagramView::WndProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam)
{
    switch (msg) {
    // 本体 (Win32Window) と同じく、開閉で DWM のアニメーションを出さない。閉じ方が多く (Esc・×・文書の切り替え・本体と
    // 一緒に) どれも破棄の中の非表示化を通るので、ここで隠す。本体の最小化でも一緒に隠れるので、解くのも表示を開く
    // 1 か所ではなくここで行う。WM_WINDOWPOSCHANGED は WM_SIZE を作るので既定の処理へ回す。
    case WM_WINDOWPOSCHANGING:
        if (reinterpret_cast<const WINDOWPOS*>(lparam)->flags & SWP_HIDEWINDOW) {
            SetCloaked(hwnd, TRUE);
        }
        break;
    case WM_WINDOWPOSCHANGED:
        if (reinterpret_cast<const WINDOWPOS*>(lparam)->flags & SWP_SHOWWINDOW) {
            UpdateWindow(hwnd);
            SetCloaked(hwnd, FALSE);
        }
        break;
    case WM_DPICHANGED: {
        const RECT& r = *reinterpret_cast<const RECT*>(lparam);
        SetWindowPos(hwnd, nullptr, r.left, r.top, r.right - r.left, r.bottom - r.top, SWP_NOZORDER | SWP_NOACTIVATE);
        return 0;
    }
    default:
        break;
    }
    auto* const self = BindWindowInstance<DiagramView>(hwnd, msg, lparam);
    if (!self) {
        return DefWindowProcW(hwnd, msg, wparam, lparam);
    }
    switch (msg) {
    case WM_CLOSE: {
        // 呼ばれた側がこのオブジェクトを壊すので、呼ぶ関数は手元に移してから呼び、後で self に触らない。
        auto on_close = std::move(self->on_close_);
        on_close();
        return 0;
    }
    case WM_NCDESTROY:
        // 本体と一緒に壊されたときも、壊れた hwnd を持ち続けない。
        self->hwnd_ = nullptr;
        break;
    default:
        if (LRESULT result = 0; self->hwnd_ && self->HandleMessage(msg, wparam, lparam, result)) {
            return result;
        }
        break;
    }
    return DefWindowProcW(hwnd, msg, wparam, lparam);
}

void DiagramView::ApplyLanguage()
{
    SetWindowTextW(hwnd_, i18n::S().viewer_title.data());
    // 倍率と読み込み中は 13px、ボタンの記号は 16px。
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
    HoverAt(Button::None);
    Invalidate();
}

void DiagramView::SetDarkMode(bool dark)
{
    dark_ = dark;
    ApplyDarkModeToWindow(hwnd_, dark);
    tooltip_.ApplyDarkMode(dark);
    Invalidate();
}

void DiagramView::ShowLoading()
{
    drawing_.reset();
    commands_.Reset();
    HoverAt(Button::None);
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
    return DpiScaleFrom(static_cast<float>(GetDpiForWindow(hwnd_)));
}

D2D1_POINT_2F DiagramView::ToDip(LPARAM lparam) const noexcept
{
    const auto [x, y] = PixelToDip(GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam), Scale());
    return { x, y };
}

bool DiagramView::EnsureTarget()
{
    if (rt_) {
        return true;
    }
    RECT rc{};
    GetClientRect(hwnd_, &rc);
    const float dpi = Scale() * DEFAULT_DPI;
    // D2D1_PRESENT_OPTIONS_IMMEDIATELY は、スクロールバーが出て窓が縮んだ直後の 1 枚が画面に出ないことがあった (2 回に 1 回)。
    if (FAILED(d2d_->CreateHwndRenderTarget(D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_DEFAULT, D2D1::PixelFormat(), dpi, dpi),
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
    const float zoom = Zoom();
    const auto place = [zoom](float size, float view, float scroll) {
        return ContentExtent(size, zoom) <= view ? (view - size * zoom) / 2.0f : kPadding - scroll;
    };
    return { place(drawing_->width, view_width_, scroll_x_), place(drawing_->height, view_height_, scroll_y_) };
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
            dc->SetTransform(D2D1::Matrix3x2F::Scale(Zoom(), Zoom()) * D2D1::Matrix3x2F::Translation(origin.x, origin.y));
            dc->DrawImage(commands_.Get());
            dc->SetTransform(D2D1::Matrix3x2F::Identity());
        }
        PaintBar();
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
    const bool vscroll = (GetWindowLongPtrW(hwnd_, GWL_STYLE) & WS_VSCROLL) != 0;
    const float reserve = vscroll ? 0.0f : PixelToDip(static_cast<float>(GetSystemMetricsForDpi(SM_CXVSCROLL, GetDpiForWindow(hwnd_))), Scale());
    const float right = view_width_ - reserve - kBarMargin;
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
        return diagram_view::NextZoom(Zoom(), fit_zoom_, 1).has_value();
    case Button::ZoomOut:
        return diagram_view::NextZoom(Zoom(), fit_zoom_, -1).has_value();
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

void DiagramView::PaintBar()
{
    ID2D1SolidColorBrush* const brush = brush_.Get();
    const Palette palette = PaletteFor(dark_);
    const D2D1_RECT_F bar = BarRect();
    brush->SetColor(palette.shadow);
    rt_->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(bar.left, bar.top + 1.0f, bar.right, bar.bottom + 1.5f), kBarCorner, kBarCorner), brush);
    brush->SetColor(palette.bar);
    rt_->FillRoundedRectangle(D2D1::RoundedRect(bar, kBarCorner, kBarCorner), brush);
    const Button highlighted = HighlightedButton();
    for (const Button b : { Button::ZoomOut, Button::ZoomIn, Button::Fit }) {
        const D2D1_RECT_F r = ButtonRect(b);
        const bool enabled = IsEnabled(b);
        if (highlighted == b && enabled) {
            brush->SetColor(palette.hover);
            rt_->FillRoundedRectangle(D2D1::RoundedRect(r, kButtonCorner, kButtonCorner), brush);
        }
        D2D1_COLOR_F glyph = palette.text;
        glyph.a = enabled ? 1.0f : DISABLED_UI_ALPHA;
        brush->SetColor(glyph);
        if (b == Button::Fit) {
            // 全体表示のアイコン (16x16 の四隅の鉤)。
            const DipPoint c = RectCenter(r);
            const float ox = c.x - 8.0f;
            const float oy = c.y - 8.0f;
            constexpr float kCorners[4][6]{
                { 1.5f,  5.5f,  1.5f,  1.5f,  5.5f,  1.5f  },
                { 10.5f, 1.5f,  14.5f, 1.5f,  14.5f, 5.5f  },
                { 14.5f, 10.5f, 14.5f, 14.5f, 10.5f, 14.5f },
                { 5.5f,  14.5f, 1.5f,  14.5f, 1.5f,  10.5f },
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
        const std::wstring zoom = std::format(L"{}%", std::lround(Zoom() * 100.0f));
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
        view_width_ = PixelToDip(static_cast<float>(rc.right), scale);
        view_height_ = PixelToDip(static_cast<float>(rc.bottom), scale);
        if (rt_) {
            rt_->SetDpi(scale * DEFAULT_DPI, scale * DEFAULT_DPI);
            rt_->Resize(D2D1::SizeU(rc.right, rc.bottom));
        }
        if (drawing_) {
            fit_zoom_ = diagram_view::FitZoom(view_width_, view_height_, drawing_->width, drawing_->height);
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
        const float max_scroll = MaxScroll(content, view);
        scroll = std::clamp(scroll, 0.0f, max_scroll);
        SCROLLINFO si{ .cbSize = sizeof(si), .fMask = SIF_RANGE | SIF_PAGE | SIF_POS };
        if (max_scroll > 0.0f) {
            si.nMax = static_cast<int>(std::ceil(content * scale)) - 1;
            si.nPage = static_cast<UINT>(std::floor(view * scale));
            si.nPos = static_cast<int>(std::lround(scroll * scale));
        }
        SetScrollInfo(hwnd_, bar, &si, TRUE);
    };
    apply(SB_HORZ, drawing_ ? ContentExtent(drawing_->width, Zoom()) : 0.0f, view_width_, scroll_x_);
    apply(SB_VERT, drawing_ ? ContentExtent(drawing_->height, Zoom()) : 0.0f, view_height_, scroll_y_);
}

void DiagramView::ScrollTo(float x, float y)
{
    if (!drawing_) {
        return;
    }
    x = std::clamp(x, 0.0f, MaxScroll(ContentExtent(drawing_->width, Zoom()), view_width_));
    y = std::clamp(y, 0.0f, MaxScroll(ContentExtent(drawing_->height, Zoom()), view_height_));
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
    const float ax = (view_width_ / 2.0f - before.x) / Zoom();
    const float ay = (view_height_ / 2.0f - before.y) / Zoom();
    manual_zoom_ = zoom;
    Layout();
    // 新しい倍率で同じ点が中央に来るスクロール位置。余白込みの面では図の左上が kPadding にある。
    ScrollTo(kPadding + ax * zoom - view_width_ / 2.0f, kPadding + ay * zoom - view_height_ / 2.0f);
}

void DiagramView::Fit()
{
    manual_zoom_.reset();
    scroll_x_ = 0.0f;
    scroll_y_ = 0.0f;
    Layout();
}

void DiagramView::Step(int dir)
{
    if (const auto next = drawing_ ? diagram_view::NextZoom(Zoom(), fit_zoom_, dir) : std::nullopt) {
        SetZoom(*next);
    }
}

void DiagramView::HoverAt(Button button)
{
    Button current = Button::None;
    if (const auto* over = std::get_if<PointerOver>(&pointer_)) {
        current = over->button;
    }
    else if (!std::holds_alternative<PointerIdle>(pointer_)) {
        return;
    }
    if (current == button) {
        return;
    }
    if (button == Button::None) {
        pointer_ = PointerIdle{};
    }
    else {
        pointer_ = PointerOver{ button };
    }
    const auto& s = i18n::S();
    tooltip_.Arm(button == Button::ZoomIn    ? s.viewer_zoom_in
                 : button == Button::ZoomOut ? s.viewer_zoom_out
                 : button == Button::Fit     ? s.viewer_fit
                                             : std::wstring_view{});
    Invalidate();
}

DiagramView::Button DiagramView::HighlightedButton() const noexcept
{
    if (const auto* over = std::get_if<PointerOver>(&pointer_)) {
        return over->button;
    }
    if (const auto* press = std::get_if<PointerPressing>(&pointer_)) {
        return press->button;
    }
    return Button::None;
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
        tooltip_.OnTimer();
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
        tooltip_.Dismiss();
        if (b != Button::None) {
            if (IsEnabled(b)) {
                pointer_ = PointerPressing{ b };
                SetCapture(hwnd_);
            }
            else {
                pointer_ = PointerOver{ b, /*clicked=*/true };
            }
            Invalidate();
        }
        else if (drawing_) {
            pointer_ = PointerDragging{ p };
            SetCapture(hwnd_);
            SetCursor(LoadCursorW(nullptr, IDC_SIZEALL));
        }
        return true;
    }

    case WM_MOUSEMOVE: {
        const auto p = ToDip(lparam);
        if (!tracking_leave_) {
            TRACKMOUSEEVENT tme{ .cbSize = sizeof(tme), .dwFlags = TME_LEAVE, .hwndTrack = hwnd_ };
            tracking_leave_ = TrackMouseEvent(&tme) != FALSE;
        }
        if (auto* drag = std::get_if<PointerDragging>(&pointer_)) {
            const D2D1_POINT_2F last = std::exchange(drag->last, p);
            ScrollBy(last.x - p.x, last.y - p.y);
        }
        else {
            HoverAt(HitButton(p.x, p.y));
        }
        return true;
    }

    case WM_LBUTTONUP: {
        const auto* press = std::get_if<PointerPressing>(&pointer_);
        if (!press && !std::holds_alternative<PointerDragging>(pointer_)) {
            return true;
        }
        const Button pressed = press ? press->button : Button::None;
        const auto p = ToDip(lparam);
        const Button hit = HitButton(p.x, p.y);
        // 段階を移してから外すので、WM_CAPTURECHANGED は何もしない。
        pointer_ = PointerIdle{};
        if (GetCapture() == hwnd_) {
            ReleaseCapture();
        }
        if (pressed != Button::None && hit == pressed) {
            pointer_ = PointerOver{ pressed, /*clicked=*/true };
            if (pressed == Button::Fit) {
                Fit();
            }
            else {
                Step(pressed == Button::ZoomIn ? 1 : -1);
            }
        }
        else {
            HoverAt(hit);
        }
        Invalidate();
        return true;
    }

    // ほかの窓にキャプチャを取られたら、押したボタンもドラッグも取り消す。
    case WM_CAPTURECHANGED:
        if (std::holds_alternative<PointerPressing>(pointer_) || std::holds_alternative<PointerDragging>(pointer_)) {
            pointer_ = PointerIdle{};
            Invalidate();
        }
        return true;

    case WM_MOUSELEAVE:
        tracking_leave_ = false;
        HoverAt(Button::None);
        return true;

    case WM_SETCURSOR:
        if (LOWORD(lparam) != HTCLIENT) {
            return false;
        }
        SetCursor(LoadCursorW(nullptr, std::holds_alternative<PointerDragging>(pointer_) ? IDC_SIZEALL : IDC_ARROW));
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

    // キーの配置に依らず、打った文字で倍率を変える。
    case WM_CHAR:
        if (IsKeyDown(VK_CONTROL) || IsKeyDown(VK_MENU)) {
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
