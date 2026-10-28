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

ID2D1RenderTarget* Renderer::GetRenderTarget() const noexcept
{
    return device_context_.Get();
}

bool Renderer::Init(HWND hwnd, const Theme& theme)
{
    theme_ = &theme;

    if (!CreateFactories(hwnd)) {
        return false;
    }

    RecreateAllTextFormats();
    DecodeAppIcon();

    cmd_generator_.Init(theme, &fmt_.md, &text_layouts_);

    return true;
}

void Renderer::TrimForMinimize() noexcept
{
    TrimDevice();
    toc_titles_ = {};
}

void Renderer::PrepareChrome(const TitleBarRenderState& titlebar, const SidePaneState& sp)
{
    EnsurePaneListFormats(PaneTarget::File, sp.Get(PaneTarget::File).zoom);
    EnsurePaneListFormats(PaneTarget::Toc, sp.Get(PaneTarget::Toc).zoom);
    chrome_cmds_.clear();
    chrome::EmitTitleBar(chrome_cmds_, text_layouts_, fmt_.ui_icon.Get(), fmt_.titlebar_text.Get(), app_icon_bitmap_.Get(), theme_->dark, titlebar);
    const auto& file_fmt = pane_list_fmt_[std::to_underlying(PaneTarget::File)];
    const auto& toc_fmt = pane_list_fmt_[std::to_underlying(PaneTarget::Toc)];
    const chrome::SidePaneFormats fmts{
        .header = fmt_.pane_header.Get(),
        .header_icon = fmt_.pane_icon.Get(),
        .item = { file_fmt.item.Get(), toc_fmt.item.Get() },
        .file_icon = file_fmt.icon.Get(),
    };
    chrome::EmitSidePanes(chrome_cmds_, text_layouts_, fmts, *theme_, sp, toc_titles_);
}

std::optional<Renderer::FrameResult> Renderer::BeginFrame(const TitleBarRenderState& titlebar, const SidePaneState& side_panes)
{
    if (device_lost_) {
        return RecreateDeviceForFrame();
    }
    if (!rt()) {
        return FrameResult::Done;
    }

    text_layouts_.BeginFrame();
    PrepareChrome(titlebar, side_panes);
    // メッセージループが取っていなければ (モーダルループや UpdateWindow からの描画) ここで待つ。
    AcquireFrame();

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
        const auto& cmds = cmd_generator_.GenerateMdPane(p.md, DpiScaleFrom(dpi_));
        MENDO_PLOT("draw.command_count", static_cast<int64_t>(cmds.size()));
        ExecuteDrawCommands(cmds, rt(), brushes_, &inline_math_);
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
        if (p.gesture.trail) {
            GenGestureTrail(*p.gesture.trail);
        }
    }
    // 読み込み中もジェスチャーとトーストは出す。
    overlay::EmitGestureOverlay(overlay_cmds_, text_layouts_, fmt_.gesture_overlay.Get(), md_pane_rect, dark, p.gesture.direction);
    overlay::EmitToast(overlay_cmds_, text_layouts_, fmt_.toast_text.Get(), md_pane_rect, dark, p.toast_message);
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
        Present();
        // REMOVED/RESET/HUNG/DRIVER_INTERNAL_ERROR で device_lost_ をセット済み。
        // 次のフレームの BeginFrame で作り直す。
        if (device_lost_) {
            return FrameResult::RedrawNow;
        }
    }
    return FrameResult::Done;
}

bool Renderer::RecreateDevice()
{
    if (!CreateDevice()) {
        return false;
    }
    inline_math_ = {};
    if (on_device_lost_) {
        on_device_lost_(rt());
    }
    return true;
}

void Renderer::GenGestureTrail(const std::vector<GesturePoint>& points)
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
