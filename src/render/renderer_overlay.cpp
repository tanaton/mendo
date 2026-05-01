#include "renderer.h"
#include "ui_constants.h"
#include <algorithm>
#include <wrl/client.h>

using Microsoft::WRL::ComPtr;

void Renderer::DrawNavOverlay(const PaneRect& md_pane_rect, bool can_back, bool can_forward, int hovered)
{
    if (!rt()) {
        return;
    }

    const bool is_dark = theme_.IsDark();

    const float base_x = md_pane_rect.x + md_pane_rect.width - NAV_BTN_MARGIN - NAV_BTN_SIZE * 2 - NAV_BTN_GAP - NAV_BTN_SCROLLBAR_OFFSET;
    const float base_y = md_pane_rect.y + md_pane_rect.height - NAV_BTN_MARGIN - NAV_BTN_SIZE;

    // SetColor は SetOpacity より重いため、固定色ブラシを is_dark で選んで
    // 透明度のみ切り替える。
    ID2D1SolidColorBrush* const overlay_brush = is_dark ? Brush(BrushId::OverlayWhite) : Brush(BrushId::OverlayBlack);

    auto drawButton = [&](float x, bool enabled, bool is_hovered, IDWriteTextLayout* arrow_layout) {
        if (!overlay_brush) {
            return;
        }
        const D2D1_RECT_F rect = D2D1::RectF(x, base_y, x + NAV_BTN_SIZE, base_y + NAV_BTN_SIZE);

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

        overlay_brush->SetOpacity(bg_alpha);
        const D2D1_ROUNDED_RECT rrect = D2D1::RoundedRect(rect, NAV_BTN_CORNER, NAV_BTN_CORNER);
        rt()->FillRoundedRectangle(rrect, overlay_brush);

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

        if (arrow_layout) {
            overlay_brush->SetOpacity(text_alpha);
            rt()->DrawTextLayout(D2D1::Point2F(x, base_y), arrow_layout, overlay_brush);
        }
    };

    drawButton(base_x, can_back, hovered == 1, nav_back_layout_.Get());
    drawButton(base_x + NAV_BTN_SIZE + NAV_BTN_GAP, can_forward, hovered == 2, nav_forward_layout_.Get());

    if (overlay_brush) {
        // 共有ブラシの状態が後続の描画に漏れないようリセットする。
        overlay_brush->SetOpacity(1.0f);
    }
}

void Renderer::DrawGestureTrail(const std::pmr::deque<GesturePoint>& points)
{
    if (!rt() || points.size() < 2) {
        return;
    }
    if (!Brush(BrushId::Overlay) || !d2d()) {
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

    const auto point_count = points.size();
    sink->BeginFigure(D2D1::Point2F(points[0].x, points[0].y), D2D1_FIGURE_BEGIN_HOLLOW);
    for (size_t i = 1; i < point_count; i++) {
        sink->AddLine(D2D1::Point2F(points[i].x, points[i].y));
    }
    sink->EndFigure(D2D1_FIGURE_END_OPEN);
    if (FAILED(sink->Close())) {
        return;
    }

    Brush(BrushId::Overlay)->SetColor(D2D1::ColorF(0.9f, 0.2f, 0.2f, 0.5f));
    if (!gesture_stroke_style_) {
        // 滑らかなジェスチャー軌跡のための丸型キャップと結合（初回のみ生成）
        const D2D1_STROKE_STYLE_PROPERTIES ssp = D2D1::StrokeStyleProperties(
            D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND,
            D2D1_CAP_STYLE_ROUND, D2D1_LINE_JOIN_ROUND);
        d2d()->CreateStrokeStyle(ssp, nullptr, 0, &gesture_stroke_style_);
    }
    rt()->DrawGeometry(path.Get(), Brush(BrushId::Overlay), GESTURE_TRAIL_STROKE_WIDTH, gesture_stroke_style_.Get());
}

void Renderer::DrawGestureOverlay(int direction, float alpha, const PaneRect& md_pane_rect)
{
    if (!rt() || direction == 0) {
        return;
    }

    const bool is_dark = theme_.IsDark();

    const float rect_w = GESTURE_OVERLAY_WIDTH;
    const float rect_h = GESTURE_OVERLAY_HEIGHT;
    const float cx = md_pane_rect.x + md_pane_rect.width / 2.0f;
    const float cy = md_pane_rect.y + md_pane_rect.height / 2.0f;
    const D2D1_RECT_F rect = D2D1::RectF(cx - rect_w / 2, cy - rect_h / 2, cx + rect_w / 2, cy + rect_h / 2);

    if (auto* bg_brush = Brush(BrushId::Overlay)) {
        const D2D1_COLOR_F bg_color = is_dark ? D2D1::ColorF(0.2f, 0.2f, 0.2f, alpha * 0.8f) : D2D1::ColorF(0.0f, 0.0f, 0.0f, alpha * 0.6f);
        bg_brush->SetColor(bg_color);
        const D2D1_ROUNDED_RECT rrect = D2D1::RoundedRect(rect, GESTURE_OVERLAY_CORNER, GESTURE_OVERLAY_CORNER);
        rt()->FillRoundedRectangle(rrect, bg_brush);
    }

    auto* gesture_layout = (direction < 0) ? gesture_back_layout_.Get() : gesture_forward_layout_.Get();
    if (gesture_layout) {
        if (auto* white = Brush(BrushId::OverlayWhite)) {
            white->SetOpacity(alpha);
            rt()->DrawTextLayout(D2D1::Point2F(rect.left, rect.top), gesture_layout, white);
            white->SetOpacity(1.0f);
        }
    }
}

void Renderer::DrawToastOverlay(const ToastRenderState& toast, const PaneRect& md_pane_rect)
{
    if (!rt() || toast.message.empty()) {
        return;
    }

    const float alpha = std::min(toast.alpha, 1.0f);
    const bool is_dark = theme_.IsDark();

    const float rect_w = TOAST_OVERLAY_WIDTH;
    const float rect_h = TOAST_OVERLAY_HEIGHT;
    const float cx = md_pane_rect.x + md_pane_rect.width / 2.0f;
    const float bottom_y = md_pane_rect.y + md_pane_rect.height - NAV_BTN_MARGIN - NAV_BTN_SIZE - TOAST_OVERLAY_BOTTOM_OFFSET;
    const D2D1_RECT_F rect = D2D1::RectF(cx - rect_w / 2, bottom_y - rect_h, cx + rect_w / 2, bottom_y);

    if (auto* bg_brush = Brush(BrushId::Overlay)) {
        const D2D1_COLOR_F bg_color = is_dark ? D2D1::ColorF(0.2f, 0.2f, 0.2f, alpha * 0.85f) : D2D1::ColorF(0.0f, 0.0f, 0.0f, alpha * 0.7f);
        bg_brush->SetColor(bg_color);
        const D2D1_ROUNDED_RECT rrect = D2D1::RoundedRect(rect, TOAST_OVERLAY_CORNER, TOAST_OVERLAY_CORNER);
        rt()->FillRoundedRectangle(rrect, bg_brush);
    }

    // メッセージ変更時のみキャッシュ済みレイアウトを再作成
    if (fmt_.toast_text) {
        if (!cached_toast_layout_ || toast.message != cached_toast_text_) {
            cached_toast_text_ = toast.message;
            cached_toast_layout_.Reset();
            backend_.GetDWriteFactory()->CreateTextLayout(
                cached_toast_text_.data(),
                static_cast<UINT32>(cached_toast_text_.size()),
                fmt_.toast_text.Get(),
                TOAST_OVERLAY_WIDTH,
                TOAST_OVERLAY_HEIGHT,
                &cached_toast_layout_);
        }
        if (cached_toast_layout_) {
            if (auto* white = Brush(BrushId::OverlayWhite)) {
                white->SetOpacity(alpha);
                rt()->DrawTextLayout(D2D1::Point2F(rect.left, rect.top), cached_toast_layout_.Get(), white);
                white->SetOpacity(1.0f);
            }
        }
    }
}
