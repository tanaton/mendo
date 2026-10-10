#include "renderer.h"
#include "hit_test_service.h"
#include "ui_constants.h"
#include "d2d_util.h"
#include <algorithm>
#include <wrl/client.h>

using Microsoft::WRL::ComPtr;

void Renderer::DrawNavOverlay(const PaneRect& md_pane_rect, bool can_back, bool can_forward, NavButtonHover hovered)
{
    const bool is_dark = theme_.IsDark();

    // SetColor は SetOpacity より重いため、固定色ブラシを is_dark で選んで
    // 透明度のみ切り替える。
    const BrushId overlay_id = is_dark ? BrushId::OverlayWhite : BrushId::OverlayBlack;
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
            mendo::OpacityScope guard{ overlay_brush, bg_alpha };
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
        DrawIcon(arrow, fmt_.nav_button.Get(), rect, overlay_id, text_alpha, D2D1::SizeF(NAV_BTN_SIZE, NAV_BTN_SIZE));
    };

    // クリック判定 (NavButtonHitTest) と同じ矩形 API を使い、描画とヒットのズレを防ぐ
    draw_button(NavBackButtonRect(md_pane_rect), can_back, hovered == NavButtonHover::Back, L"\x25C0");
    draw_button(NavForwardButtonRect(md_pane_rect), can_forward, hovered == NavButtonHover::Forward, L"\x25B6");
}

void Renderer::DrawGestureTrail(const std::pmr::deque<GesturePoint>& points)
{
    if (points.size() < 2) {
        return;
    }
    auto* const trail_brush = Brush(BrushId::GestureTrail);
    if (!trail_brush || !d2d()) {
        return;
    }

    // パスジオメトリで一筆描きすることで、結合部のアルファ蓄積（節）を防ぐ
    ComPtr<ID2D1PathGeometry> path;
    if (FAILED(d2d()->CreatePathGeometry(&path))) {
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
        const D2D1_STROKE_STYLE_PROPERTIES ssp = D2D1::StrokeStyleProperties(
            D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND,
            D2D1_CAP_STYLE_ROUND, D2D1_LINE_JOIN_ROUND);
        d2d()->CreateStrokeStyle(ssp, nullptr, 0, &gesture_stroke_style_);
    }
    mendo::OpacityScope guard{ trail_brush, 0.5f };
    rt()->DrawGeometry(path.Get(), trail_brush, GESTURE_TRAIL_STROKE_WIDTH, gesture_stroke_style_.Get());
}

void Renderer::DrawGestureOverlay(int direction, const PaneRect& md_pane_rect)
{
    if (direction == 0) {
        return;
    }

    const float rect_w = GESTURE_OVERLAY_WIDTH;
    const float rect_h = GESTURE_OVERLAY_HEIGHT;
    const float cx = md_pane_rect.x + md_pane_rect.width / 2.0f;
    const float cy = md_pane_rect.y + md_pane_rect.height / 2.0f;
    const D2D1_RECT_F rect = D2D1::RectF(cx - rect_w / 2, cy - rect_h / 2, cx + rect_w / 2, cy + rect_h / 2);
    FillOverlayPanel(rect, GESTURE_OVERLAY_CORNER, theme_.IsDark() ? 0.8f : 0.6f);

    DrawIcon((direction < 0) ? L"\x2190 \x623B\x308B" : L"\x2192 \x9032\x3080", fmt_.gesture_overlay.Get(), rect, BrushId::OverlayWhite, 1.0f, D2D1::SizeF(rect_w, rect_h));
}

void Renderer::FillOverlayPanel(const D2D1_RECT_F& rect, float corner, float alpha)
{
    // dark テーマは灰色 (0.2,0.2,0.2)、light テーマは純黒 (0,0,0) で背景。
    // 専用色ブラシ + OpacityScope で毎フレーム SetColor を回避する。
    auto* const bg_brush = theme_.IsDark() ? Brush(BrushId::OverlayGestureBg) : Brush(BrushId::OverlayBlack);
    if (!bg_brush) {
        return;
    }
    mendo::OpacityScope guard{ bg_brush, alpha };
    rt()->FillRoundedRectangle(D2D1::RoundedRect(rect, corner, corner), bg_brush);
}

void Renderer::DrawToastOverlay(const ToastRenderState& toast, const PaneRect& md_pane_rect)
{
    if (toast.message.empty()) {
        return;
    }

    const float alpha = std::min(toast.alpha, 1.0f);

    const float rect_w = TOAST_OVERLAY_WIDTH;
    const float rect_h = TOAST_OVERLAY_HEIGHT;
    const float cx = md_pane_rect.x + md_pane_rect.width / 2.0f;
    const float bottom_y = md_pane_rect.y + md_pane_rect.height - NAV_BTN_MARGIN - NAV_BTN_SIZE - TOAST_OVERLAY_BOTTOM_OFFSET;
    const D2D1_RECT_F rect = D2D1::RectF(cx - rect_w / 2, bottom_y - rect_h, cx + rect_w / 2, bottom_y);

    FillOverlayPanel(rect, TOAST_OVERLAY_CORNER, alpha * (theme_.IsDark() ? 0.85f : 0.7f));

    DrawCenteredText(toast_layout_, toast.message, fmt_.toast_text.Get(), rect, BrushId::OverlayWhite, alpha, D2D1::SizeF(rect_w, rect_h));
}
