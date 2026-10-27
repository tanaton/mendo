#include "renderer.h"
#include "d2d_util.h"
#include "mouse_gesture.h"
#include <d2d1_1.h>
#include <wincodec.h>
#include "ui_constants.h"
#include "profiler.h"
#include <utility>
#include <wrl/client.h>

using Microsoft::WRL::ComPtr;

Renderer::Renderer() noexcept = default;
Renderer::~Renderer() = default;

ID2D1RenderTarget* Renderer::GetRenderTarget() const noexcept
{
    return backend_.GetRenderTarget();
}

bool Renderer::Init(HWND hwnd, const Theme& theme)
{
    theme_ = &theme;

    if (!backend_.Init(hwnd)) {
        return false;
    }

    RecreateAllTextFormats();
    DecodeAppIcon();

    cmd_generator_.Init(theme, &fmt_.md, &text_layouts_);

    return true;
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

void Renderer::PrepareChrome(const TitleBarRenderState& titlebar, const SidePaneState& sp)
{
    EnsurePaneListFormats(PaneTarget::File, sp.Get(PaneTarget::File).zoom);
    EnsurePaneListFormats(PaneTarget::Toc, sp.Get(PaneTarget::Toc).zoom);
    chrome_cmds_.clear();
    GenTitleBar(titlebar);

    const auto gen_splitter = [&](const PaneRect& r) {
        const float x = r.x + r.width;
        chrome_cmds_.push_back(FillRectCmd{ .rect = D2D1::RectF(x, r.y, x + theme_->splitter_width, r.y + r.height), .brush_id = BrushId::Splitter });
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
    rt()->Clear(theme_->Color(BrushId::Background));
    ExecuteDrawCommands(chrome_cmds_, rt(), brushes_);
    return std::nullopt;
}

Renderer::FrameResult Renderer::Render(const RenderParams& p)
{
    MENDO_PROFILE("Render");

    if (const auto early = BeginFrame(p.titlebar, p.side_panes)) {
        return *early;
    }

    if (!p.loading_angle) {
        const auto& cmds = cmd_generator_.GenerateMdPane(p.md, DpiScaleFrom(backend_.GetDpi()));
        MENDO_PLOT("draw.command_count", static_cast<int64_t>(cmds.size()));
        ExecuteDrawCommands(cmds, rt(), brushes_);
    }
    GenOverlays(p);
    ExecuteDrawCommands(overlay_cmds_, rt(), brushes_);

    return EndFrame();
}

void Renderer::GenOverlays(const RenderParams& p)
{
    overlay_cmds_.clear();
    gesture_trail_.Reset();
    const PaneRect& md_pane_rect = p.md.md_pane_rect;
    const bool dark = theme_->dark;
    if (p.loading_angle) {
        overlay::EmitLoadingSpinner(overlay_cmds_, *p.loading_angle, md_pane_rect);
    }
    else {
        if (p.can_go_back || p.can_go_forward) {
            overlay::EmitNavButtons(overlay_cmds_, text_layouts_, fmt_.nav_button.Get(), md_pane_rect, dark, p.can_go_back, p.can_go_forward, p.nav_hovered);
        }
        if (p.gesture.trail_active && p.gesture.trail_points) {
            GenGestureTrail(*p.gesture.trail_points);
        }
    }
    // 読み込み中もジェスチャーとトーストは出す。
    overlay::EmitGestureOverlay(overlay_cmds_, text_layouts_, fmt_.gesture_overlay.Get(), md_pane_rect, dark, p.gesture.direction);
    overlay::EmitToast(overlay_cmds_, text_layouts_, fmt_.toast_text.Get(), md_pane_rect, dark, p.toast.message);
    if (p.loading_angle) {
        return;
    }
    if (p.search_bar.visible) {
        const auto input = search_input_.Measure(p.search_bar, ComputeSearchBarLayout(md_pane_rect, !p.search_bar.query.empty()).text_width());
        overlay::EmitSearchBar(overlay_cmds_, text_layouts_, fmt_.ui_icon.Get(), fmt_.search_count.Get(), md_pane_rect, p.search_bar, input);
    }
    if (const auto thumb = VScrollThumb(md_pane_rect, 0.0f, p.md.scroll_y, ComputeTotalContentHeight(p.md.cache, theme_->margin_bottom), p.md.cache.HasDirtyNodes())) {
        overlay_cmds_.push_back(*thumb);
    }
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
        text_layouts_.EmitCentered(chrome_cmds_, { &icon, 1 }, fmt_.ui_icon.Get(), rect, text_id, alpha);
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
    gen_plain(TitleBarHitZone::Settings, L'\uE713');
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
        text_layouts_.EmitCentered(chrome_cmds_, tb.title_text, fmt_.titlebar_text.Get(), ToD2DRect(bar.GetTitleTextRect()), BrushId::TitleBarText, text_alpha);
    }
}

void Renderer::GenGestureTrail(const std::pmr::vector<GesturePoint>& points)
{
    auto* const factory = GetD2DFactory();
    if (points.size() < 2 || !factory) {
        return;
    }
    // パスジオメトリで一筆描きすることで、結合部のアルファ蓄積（節）を防ぐ
    ComPtr<ID2D1GeometrySink> sink;
    if (FAILED(factory->CreatePathGeometry(&gesture_trail_)) || FAILED(gesture_trail_->Open(&sink))) {
        gesture_trail_.Reset();
        return;
    }
    sink->BeginFigure(D2D1::Point2F(points[0].x, points[0].y), D2D1_FIGURE_BEGIN_HOLLOW);
    for (size_t i = 1; i < points.size(); i++) {
        sink->AddLine(D2D1::Point2F(points[i].x, points[i].y));
    }
    sink->EndFigure(D2D1_FIGURE_END_OPEN);
    if (FAILED(sink->Close())) {
        gesture_trail_.Reset();
        return;
    }

    if (!gesture_stroke_style_) {
        // 滑らかなジェスチャー軌跡のための丸型キャップと結合（初回のみ生成）
        const D2D1_STROKE_STYLE_PROPERTIES ssp = D2D1::StrokeStyleProperties(D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND, D2D1_LINE_JOIN_ROUND);
        factory->CreateStrokeStyle(ssp, nullptr, 0, &gesture_stroke_style_);
    }
    overlay_cmds_.push_back(DrawGeometryCmd{
        .geometry = gesture_trail_.Get(),
        .stroke_style = gesture_stroke_style_.Get(),
        .stroke_width = GESTURE_TRAIL_STROKE_WIDTH,
        .brush_id = BrushId::GestureTrail,
        .opacity = 0.5f,
    });
}
