#include "renderer.h"
#include "d2d_util.h"
#include "hit_test_service.h"
#include "i18n.h"
#include "ui_constants.h"
#include "profiler.h"
#include <array>
#include <cassert>
#include <cmath>
#include <utility>
#include <wrl/client.h>

using Microsoft::WRL::ComPtr;

bool Renderer::Init(HWND hwnd, Theme theme)
{
    theme_ = std::move(theme);

    if (!backend_.Init(hwnd)) {
        return false;
    }

    ResolveThemeFonts();
    RecreatePaneFormats();
    DecodeAppIcon();

    measurer_.SetFactory(backend_.GetDWriteFactory());
    if (!layout_.Init(&measurer_, theme_)) {
        return false;
    }

    cmd_generator_.SetTheme(&theme_);
    cmd_generator_.SetTextLayoutCache(&text_layouts_);

    return true;
}

void Renderer::SetTheme(Theme theme)
{
    assert(theme.zoom == theme_.zoom && theme.font_size_body == theme_.font_size_body);
    AdoptThemeKeepingFonts(std::move(theme));
    RecreateBrushes();
}

void Renderer::Resize(UINT width, UINT height, bool live) noexcept
{
    backend_.Resize(width, height, live);
}

void Renderer::SetDpi(float dpi) noexcept
{
    backend_.SetDpi(dpi);
}

void Renderer::TrimForMinimize() noexcept
{
    backend_.Trim();
    toc_titles_ = {};
}

void Renderer::ApplyZoomFromBase(const Theme& base_theme, float new_zoom)
{
    AdoptThemeKeepingFonts(base_theme);
    if (new_zoom != 1.0f) {
        theme_.ApplyZoom(new_zoom);
    }
    layout_.UpdateTheme(theme_);
    layout_.RecreateFormats();
    RecreatePaneFormats();
}

void Renderer::PrepareChrome(const TitleBarRenderState& titlebar, const SidePaneState& sp)
{
    chrome_cmds_.clear();
    GenTitleBar(titlebar);

    const auto gen_splitter = [&](const PaneRect& r) {
        const float x = r.x + r.width;
        chrome_cmds_.push_back(FillRectCmd{ .rect = D2D1::RectF(x, r.y, x + theme_.splitter_width, r.y + r.height), .brush_id = BrushId::Splitter });
    };
    const auto& fp = sp.Get(PaneTarget::File);
    if (fp.show) {
        GenFileExplorer(sp.file_entries, fp);
        gen_splitter(fp.rect);
    }
    const auto& tp = sp.Get(PaneTarget::Toc);
    if (tp.show) {
        GenToc(sp, tp);
        gen_splitter(tp.rect);
    }
}

std::optional<Renderer::FrameResult> Renderer::BeginFrame(const TitleBarRenderState& titlebar, const SidePaneState& side_panes)
{
    if (backend_.IsDeviceLost()) {
        return RecreateDeviceForFrame();
    }
    if (!rt()) {
        return FrameResult::Done;
    }

    text_layouts_.BeginFrame();
    PrepareChrome(titlebar, side_panes);
    // メッセージループが取っていなければ (モーダルループや UpdateWindow からの描画) ここで待つ。
    backend_.AcquireFrame();

    rt()->BeginDraw();
    rt()->Clear(theme_.bg_color);
    ExecuteDrawCommands(chrome_cmds_, rt(), &brushes_);
    return std::nullopt;
}

void Renderer::DrawLoadingSpinner(float angle, const PaneRect& md_pane_rect)
{
    auto* const text_brush = Brush(BrushId::Text);
    if (!text_brush) {
        return;
    }
    // alpha はドットインデックスのみに依存するためコンパイル時に決定する。
    static constexpr auto kSpinnerAlphas = []() static noexcept {
        std::array<float, spinner::DOT_COUNT> a{};
        for (int i = 0; i < spinner::DOT_COUNT; ++i) {
            a[i] = 1.0f - i * (spinner::DOT_FADE_FACTOR / spinner::DOT_COUNT);
        }
        return a;
    }();
    const float cx = md_pane_rect.x + md_pane_rect.width / 2.0f;
    const float cy = md_pane_rect.y + md_pane_rect.height / 2.0f;
    // ループ内で毎回 SetOpacity を上書きする。guard は scope 終了時の 1.0f 復帰のみ担う。
    OpacityScope guard{ text_brush, 1.0f };
    for (int i = 0; i < spinner::DOT_COUNT; i++) {
        const float a = angle - i * (TWO_PI / spinner::DOT_COUNT);
        const float dx = cx + spinner::RADIUS * std::cos(a);
        const float dy = cy + spinner::RADIUS * std::sin(a);

        const D2D1_ELLIPSE ellipse = D2D1::Ellipse(D2D1::Point2F(dx, dy), spinner::DOT_RADIUS, spinner::DOT_RADIUS);
        text_brush->SetOpacity(kSpinnerAlphas[i]);
        rt()->FillEllipse(ellipse, text_brush);
    }
}

Renderer::FrameResult Renderer::DrawLoading(
    float angle,
    const PaneRect& md_pane_rect,
    const SidePaneState& sp,
    const TitleBarRenderState& titlebar,
    const GestureRenderState& gesture,
    const ToastRenderState& toast)
{
    if (const auto early = BeginFrame(titlebar, sp)) {
        return *early;
    }

    DrawLoadingSpinner(angle, md_pane_rect);
    // ローディング中もジェスチャーとトーストは表示する
    DrawTransientOverlays(gesture, toast, md_pane_rect);

    return EndFrame();
}

void Renderer::DrawTransientOverlays(const GestureRenderState& gesture, const ToastRenderState& toast, const PaneRect& md_pane_rect)
{
    DrawGestureOverlay(gesture.direction, md_pane_rect);
    if (!toast.message.empty()) {
        DrawToastOverlay(toast, md_pane_rect);
    }
}

Renderer::FrameResult Renderer::Render(const RenderParams& p)
{
    MENDO_PROFILE("Render");

    if (const auto early = BeginFrame(p.titlebar, p.side_panes)) {
        return *early;
    }

    {
        const float dpi_scale = DpiScaleFrom(backend_.GetDpi());
        const auto& cmds = cmd_generator_.GenerateMdPane(p.nodes, p.cache, p.md_pane_rect, p.scroll_y, p.selection, p.hovered, dpi_scale, p.block_h_scroll);
        MENDO_PLOT("draw.command_count", static_cast<int64_t>(cmds.size()));
        ExecuteDrawCommands(cmds, rt(), &brushes_);
    }

    if (p.can_go_back || p.can_go_forward) {
        DrawNavOverlay(p.md_pane_rect, p.can_go_back, p.can_go_forward, p.nav_hovered);
    }

    if (p.gesture.trail_active && p.gesture.trail_points) {
        DrawGestureTrail(*p.gesture.trail_points);
    }

    DrawTransientOverlays(p.gesture, p.toast, p.md_pane_rect);

    if (p.search_bar.visible) {
        DrawSearchBar(p.search_bar, p.md_pane_rect);
    }

    DrawMdScrollbar(p.md_pane_rect, p.scroll_y, p.total_content_height, p.has_dirty_nodes);

    return EndFrame();
}

Renderer::FrameResult Renderer::EndFrame()
{
    const HRESULT hr = rt()->EndDraw();
    if (hr == D2DERR_RECREATE_TARGET) {
        return RecreateDeviceForFrame();
    }
    if (SUCCEEDED(hr)) {
        backend_.Present();
        // REMOVED/RESET/HUNG/DRIVER_INTERNAL_ERROR で backend が device_lost_ をセット済み。
        // 次のフレームの BeginFrame で作り直す。
        if (backend_.IsDeviceLost()) {
            return FrameResult::RedrawNow;
        }
    }
    return FrameResult::Done;
}

bool Renderer::CreateDevice()
{
    if (!backend_.CreateDevice()) {
        return false;
    }
    RecreateBrushes();
    LoadAppIconBitmap();
    return true;
}

bool Renderer::RecreateDevice()
{
    // 旧レンダーターゲットに紐付いたブラシを捨てる
    InvalidateBrushes();
    if (!CreateDevice()) {
        return false;
    }
    if (on_device_lost_) {
        on_device_lost_(rt());
    }
    return true;
}

void Renderer::GenTitleBar(const TitleBarRenderState& tb)
{
    const TitleBar& bar = tb.bar;
    // 完全不透明でガラス効果を隠す
    chrome_cmds_.push_back(FillRectCmd{ .rect = D2D1::RectF(0.0f, 0.0f, tb.window_width, bar.GetHeight()), .brush_id = BrushId::TitleBarBg });

    const float text_alpha = tb.window_active ? 1.0f : 0.5f;
    const auto is_hovered = [&](TitleBarHitZone z) noexcept { return tb.hovered_zone == z; };

    const auto gen_button = [&](const DipRect& dip_rect, wchar_t icon, bool show_bg, BrushId bg_id, BrushId text_id, float alpha) {
        const D2D1_RECT_F rect = ToD2DRect(dip_rect);
        if (show_bg) {
            chrome_cmds_.push_back(FillRectCmd{ .rect = rect, .brush_id = bg_id });
        }
        EmitChromeText({ &icon, 1 }, fmt_.titlebar_icon.Get(), rect, text_id, alpha);
    };

    const auto gen_plain = [&](TitleBarHitZone zone, wchar_t icon) {
        gen_button(bar.GetButton(zone), icon, is_hovered(zone), BrushId::TitleBarButtonHover, BrushId::TitleBarText, text_alpha);
    };
    // active > hover の優先度
    const auto gen_toggle = [&](TitleBarHitZone zone, wchar_t icon, bool active) {
        gen_button(
            bar.GetButton(zone),
            icon,
            active || is_hovered(zone),
            active ? BrushId::TitleBarButtonActive : BrushId::TitleBarButtonHover,
            BrushId::TitleBarText,
            text_alpha);
    };

    gen_plain(TitleBarHitZone::OpenFile, L'\uE838');
    gen_plain(TitleBarHitZone::Help, L'\uE897');
    gen_plain(TitleBarHitZone::ThemeToggle, tb.is_dark_mode ? L'\uE706' : L'\uE708');
    gen_toggle(TitleBarHitZone::Search, L'\uE721', tb.search_active);
    gen_toggle(TitleBarHitZone::FileToggle, L'\uE8B7', tb.file_pane_visible);
    gen_toggle(TitleBarHitZone::TocToggle, L'\uE8FD', tb.toc_pane_visible);
    gen_plain(TitleBarHitZone::Minimize, L'\uE921');
    gen_plain(TitleBarHitZone::Maximize, tb.is_maximized ? L'\uE923' : L'\uE922');
    if (is_hovered(TitleBarHitZone::Close)) {
        gen_button(bar.GetButton(TitleBarHitZone::Close), L'\uE8BB', true, BrushId::TitleBarCloseRed, BrushId::OverlayWhite, 1.0f);
    }
    else {
        gen_plain(TitleBarHitZone::Close, L'\uE8BB');
    }

    if (app_icon_bitmap_) {
        chrome_cmds_.push_back(DrawBitmapCmd{ .bitmap = app_icon_bitmap_.Get(), .dest = ToD2DRect(bar.GetIconRect()), .opacity = text_alpha });
    }

    if (!tb.title_text.empty()) {
        EmitChromeText(tb.title_text, fmt_.titlebar_text.Get(), ToD2DRect(bar.GetTitleTextRect()), BrushId::TitleBarText, text_alpha);
    }
}

void Renderer::EmitChromeText(std::wstring_view text, IDWriteTextFormat* fmt, const D2D1_RECT_F& rect, BrushId brush_id, float opacity, D2D1_SIZE_F box)
{
    if (auto* layout = text_layouts_.Get(fmt, text, box)) {
        chrome_cmds_.push_back(DrawTextLayoutCmd{ .origin = TextLayoutCache::Origin(rect, box), .layout = layout, .brush_id = brush_id, .opacity = opacity });
    }
}

void Renderer::DrawCenteredText(std::wstring_view text, IDWriteTextFormat* fmt, const D2D1_RECT_F& rect, BrushId brush_id, float alpha, D2D1_SIZE_F box)
{
    auto* const brush = Brush(brush_id);
    auto* const layout = text_layouts_.Get(fmt, text, box);
    if (!brush || !layout) {
        return;
    }
    OpacityScope guard{ brush, alpha };
    rt()->DrawTextLayout(TextLayoutCache::Origin(rect, box), layout, brush);
}

void Renderer::DrawNavOverlay(const PaneRect& md_pane_rect, bool can_back, bool can_forward, NavDirection hovered)
{
    const bool is_dark = theme_.IsDark();

    // SetColor は SetOpacity より重いため、固定色ブラシを is_dark で選んで
    // 透明度のみ切り替える。
    const BrushId overlay_id = OverlayBrushId(is_dark);
    ID2D1SolidColorBrush* const overlay_brush = Brush(overlay_id);

    if (!overlay_brush) {
        return;
    }

    const auto draw_button = [&](const D2D1_RECT_F& rect, bool enabled, bool is_hovered, std::wstring_view arrow) {
        float bg_alpha;
        if (!enabled) {
            bg_alpha = is_dark ? 0.08f : 0.05f;
        }
        else if (is_hovered) {
            bg_alpha = is_dark ? 0.35f : 0.25f;
        }
        else {
            bg_alpha = is_dark ? 0.15f : 0.10f;
        }

        {
            OpacityScope guard{ overlay_brush, bg_alpha };
            const D2D1_ROUNDED_RECT rrect = D2D1::RoundedRect(rect, NAV_BTN_CORNER, NAV_BTN_CORNER);
            rt()->FillRoundedRectangle(rrect, overlay_brush);
        }

        float text_alpha;
        if (!enabled) {
            text_alpha = is_dark ? 0.2f : 0.15f;
        }
        else if (is_hovered) {
            text_alpha = 1.0f;
        }
        else {
            text_alpha = is_dark ? 0.6f : 0.5f;
        }
        DrawCenteredText(arrow, fmt_.nav_button.Get(), rect, overlay_id, text_alpha, D2D1::SizeF(NAV_BTN_SIZE, NAV_BTN_SIZE));
    };

    // クリック判定 (NavButtonHitTest) と同じ矩形 API を使い、描画とヒットのズレを防ぐ
    draw_button(NavBackButtonRect(md_pane_rect), can_back, hovered == NavDirection::Back, L"\x25C0");
    draw_button(NavForwardButtonRect(md_pane_rect), can_forward, hovered == NavDirection::Forward, L"\x25B6");
}

void Renderer::DrawGestureTrail(const std::pmr::vector<GesturePoint>& points)
{
    if (points.size() < 2) {
        return;
    }
    auto* const trail_brush = Brush(BrushId::GestureTrail);
    auto* const factory = GetD2DFactory();
    if (!trail_brush || !factory) {
        return;
    }

    // パスジオメトリで一筆描きすることで、結合部のアルファ蓄積（節）を防ぐ
    ComPtr<ID2D1PathGeometry> path;
    if (FAILED(factory->CreatePathGeometry(&path))) {
        return;
    }

    ComPtr<ID2D1GeometrySink> sink;
    if (FAILED(path->Open(&sink))) {
        return;
    }

    sink->BeginFigure(D2D1::Point2F(points[0].x, points[0].y), D2D1_FIGURE_BEGIN_HOLLOW);
    for (size_t i = 1; i < points.size(); i++) {
        sink->AddLine(D2D1::Point2F(points[i].x, points[i].y));
    }
    sink->EndFigure(D2D1_FIGURE_END_OPEN);
    if (FAILED(sink->Close())) {
        return;
    }

    if (!gesture_stroke_style_) {
        // 滑らかなジェスチャー軌跡のための丸型キャップと結合（初回のみ生成）
        const D2D1_STROKE_STYLE_PROPERTIES ssp = D2D1::StrokeStyleProperties(D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND, D2D1_LINE_JOIN_ROUND);
        factory->CreateStrokeStyle(ssp, nullptr, 0, &gesture_stroke_style_);
    }
    OpacityScope guard{ trail_brush, 0.5f };
    rt()->DrawGeometry(path.Get(), trail_brush, GESTURE_TRAIL_STROKE_WIDTH, gesture_stroke_style_.Get());
}

void Renderer::DrawGestureOverlay(NavDirection direction, const PaneRect& md_pane_rect)
{
    if (direction == NavDirection::None) {
        return;
    }

    const float rect_w = GESTURE_OVERLAY_WIDTH;
    const float rect_h = GESTURE_OVERLAY_HEIGHT;
    const float cx = md_pane_rect.x + md_pane_rect.width / 2.0f;
    const float cy = md_pane_rect.y + md_pane_rect.height / 2.0f;
    const D2D1_RECT_F rect = D2D1::RectF(cx - rect_w / 2, cy - rect_h / 2, cx + rect_w / 2, cy + rect_h / 2);
    FillOverlayPanel(rect, GESTURE_OVERLAY_CORNER, theme_.IsDark() ? 0.8f : 0.6f);

    DrawCenteredText((direction == NavDirection::Back) ? i18n::S().gesture_back : i18n::S().gesture_forward, fmt_.gesture_overlay.Get(), rect, BrushId::OverlayWhite, 1.0f, D2D1::SizeF(rect_w, rect_h));
}

void Renderer::FillOverlayPanel(const D2D1_RECT_F& rect, float corner, float alpha)
{
    // dark テーマは灰色 (0.2,0.2,0.2)、light テーマは純黒 (0,0,0) で背景。
    // 専用色ブラシ + OpacityScope で毎フレーム SetColor を回避する。
    auto* const bg_brush{ Brush(theme_.IsDark() ? BrushId::OverlayGestureBg : BrushId::OverlayBlack) };
    if (!bg_brush) {
        return;
    }
    OpacityScope guard{ bg_brush, alpha };
    rt()->FillRoundedRectangle(D2D1::RoundedRect(rect, corner, corner), bg_brush);
}

void Renderer::DrawToastOverlay(const ToastRenderState& toast, const PaneRect& md_pane_rect)
{
    const float rect_w = TOAST_OVERLAY_WIDTH;
    const float rect_h = TOAST_OVERLAY_HEIGHT;
    const float cx = md_pane_rect.x + md_pane_rect.width / 2.0f;
    const float bottom_y = md_pane_rect.y + md_pane_rect.height - NAV_BTN_MARGIN - NAV_BTN_SIZE - TOAST_OVERLAY_BOTTOM_OFFSET;
    const D2D1_RECT_F rect = D2D1::RectF(cx - rect_w / 2, bottom_y - rect_h, cx + rect_w / 2, bottom_y);

    FillOverlayPanel(rect, TOAST_OVERLAY_CORNER, theme_.IsDark() ? 0.85f : 0.7f);

    DrawCenteredText(toast.message, fmt_.toast_text.Get(), rect, BrushId::OverlayWhite, 1.0f, D2D1::SizeF(rect_w, rect_h));
}
