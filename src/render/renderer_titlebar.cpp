#include "renderer.h"
#include "d2d_util.h"
#include <algorithm>

void Renderer::DrawTitleBar(const TitleBarRenderState& tb)
{
    if (tb.height <= 0.0f) {
        return;
    }

    // 完全不透明でガラス効果を隠す
    const D2D1_RECT_F bg_rect = D2D1::RectF(0.0f, 0.0f, tb.window_width, tb.height);
    rt()->FillRectangle(bg_rect, Brush(BrushId::TitleBarBg));

    const float text_alpha = tb.window_active ? 1.0f : 0.5f;
    const auto is_hovered = [&](TitleBarHitZone z) noexcept { return tb.hovered_zone == z; };

    const auto draw_button = [&](const DipRect& dip_rect, wchar_t icon, bool show_bg, BrushId bg_id, BrushId text_id, float alpha) {
        const D2D1_RECT_F rect = ToD2DRect(dip_rect);
        if (show_bg) {
            rt()->FillRectangle(rect, Brush(bg_id));
        }
        DrawCenteredText({ &icon, 1 }, fmt_.titlebar_icon.Get(), rect, text_id, alpha);
    };

    const auto draw_plain = [&](TitleBarHitZone zone, wchar_t icon) {
        draw_button(tb.Button(zone), icon, is_hovered(zone), BrushId::TitleBarButtonHover, BrushId::TitleBarText, text_alpha);
    };
    // active > hover の優先度
    const auto draw_toggle = [&](TitleBarHitZone zone, wchar_t icon, bool active) {
        draw_button(
            tb.Button(zone),
            icon,
            active || is_hovered(zone),
            active ? BrushId::TitleBarButtonActive : BrushId::TitleBarButtonHover,
            BrushId::TitleBarText,
            text_alpha);
    };

    draw_plain(TitleBarHitZone::OpenFile, L'\uE838');
    draw_plain(TitleBarHitZone::Help, L'\uE897');
    draw_plain(TitleBarHitZone::ThemeToggle, tb.is_dark_mode ? L'\uE706' : L'\uE708');
    draw_toggle(TitleBarHitZone::Search, L'\uE721', tb.search_active);
    draw_toggle(TitleBarHitZone::FileToggle, L'\uE8B7', tb.file_pane_visible);
    draw_toggle(TitleBarHitZone::TocToggle, L'\uE8FD', tb.toc_pane_visible);
    draw_plain(TitleBarHitZone::Minimize, L'\uE921');
    draw_plain(TitleBarHitZone::Maximize, tb.is_maximized ? L'\uE923' : L'\uE922');
    if (is_hovered(TitleBarHitZone::Close)) {
        draw_button(tb.Button(TitleBarHitZone::Close), L'\uE8BB', true, BrushId::TitleBarCloseRed, BrushId::TitleBarCloseWhite, 1.0f);
    }
    else {
        draw_plain(TitleBarHitZone::Close, L'\uE8BB');
    }

    if (app_icon_bitmap_) {
        rt()->DrawBitmap(app_icon_bitmap_.Get(), ToD2DRect(tb.icon_rect), text_alpha, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
    }

    if (!tb.title_text.empty()) {
        DrawCenteredText(tb.title_text, fmt_.titlebar_text.Get(), ToD2DRect(tb.title_text_rect), BrushId::TitleBarText, text_alpha);
    }
}

void Renderer::DrawCenteredText(std::wstring_view text, IDWriteTextFormat* fmt, const D2D1_RECT_F& rect, BrushId brush_id, float alpha, D2D1_SIZE_F box)
{
    auto* const brush = Brush(brush_id);
    mendo::OpacityScope guard{ brush, alpha };
    text_layouts_.Draw(rt(), fmt, text, rect, brush, box);
}
