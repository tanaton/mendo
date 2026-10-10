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
        DrawIcon({ &icon, 1 }, fmt_.titlebar_icon.Get(), rect, text_id, alpha);
    };

    const auto draw_plain = [&](const DipRect& rect, wchar_t icon, TitleBarHitZone zone) {
        draw_button(rect, icon, is_hovered(zone), BrushId::TitleBarButtonHover, BrushId::TitleBarText, text_alpha);
    };
    // active > hover の優先度
    const auto draw_toggle = [&](const DipRect& rect, wchar_t icon, bool active, TitleBarHitZone zone) {
        draw_button(
            rect,
            icon,
            active || is_hovered(zone),
            active ? BrushId::TitleBarButtonActive : BrushId::TitleBarButtonHover,
            BrushId::TitleBarText,
            text_alpha);
    };

    draw_plain(tb.open_file, L'\uE838', TitleBarHitZone::OpenFile);
    draw_plain(tb.help, L'\uE897', TitleBarHitZone::Help);
    draw_plain(tb.theme_toggle, tb.is_dark_mode ? L'\uE706' : L'\uE708', TitleBarHitZone::ThemeToggle);
    draw_toggle(tb.search, L'\uE721', tb.search_active, TitleBarHitZone::Search);
    draw_toggle(tb.file_toggle, L'\uE8B7', tb.file_pane_visible, TitleBarHitZone::FileToggle);
    draw_toggle(tb.toc_toggle, L'\uE8FD', tb.toc_pane_visible, TitleBarHitZone::TocToggle);
    draw_plain(tb.minimize, L'\uE921', TitleBarHitZone::Minimize);
    draw_plain(tb.maximize, tb.is_maximized ? L'\uE923' : L'\uE922', TitleBarHitZone::Maximize);
    if (is_hovered(TitleBarHitZone::Close)) {
        draw_button(tb.close, L'\uE8BB', true, BrushId::TitleBarCloseRed, BrushId::TitleBarCloseWhite, 1.0f);
    }
    else {
        draw_plain(tb.close, L'\uE8BB', TitleBarHitZone::Close);
    }

    if (app_icon_bitmap_) {
        rt()->DrawBitmap(app_icon_bitmap_.Get(), ToD2DRect(tb.icon_rect), text_alpha, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
    }

    if (!tb.title_text.empty()) {
        DrawCenteredText(title_layout_, tb.title_text, fmt_.titlebar_text.Get(), ToD2DRect(tb.title_text_rect), BrushId::TitleBarText, text_alpha);
    }
}

void Renderer::DrawCenteredText(CenteredTextLayout& slot, std::wstring_view text, IDWriteTextFormat* fmt, const D2D1_RECT_F& rect, BrushId brush_id, float alpha, D2D1_SIZE_F box)
{
    auto* const brush = Brush(brush_id);
    auto* const dw = backend_.GetDWriteFactory();
    if (!fmt || !brush || !dw) {
        return;
    }
    if (!slot.layout || slot.format != fmt || slot.text != text || slot.box.width != box.width || slot.box.height != box.height) {
        slot.layout.Reset();
        slot.format = fmt;
        slot.text.assign(text);
        slot.box = box;
        if (FAILED(dw->CreateTextLayout(text.data(), static_cast<UINT32>(text.size()), fmt, box.width, box.height, &slot.layout))) {
            return;
        }
    }
    const D2D1_POINT_2F origin = (box.width > 0.0f) ? D2D1::Point2F(rect.left, rect.top) : D2D1::Point2F((rect.left + rect.right) * 0.5f, (rect.top + rect.bottom) * 0.5f);
    mendo::OpacityScope guard{ brush, alpha };
    rt()->DrawTextLayout(origin, slot.layout.Get(), brush);
}

void Renderer::DrawIcon(std::wstring_view icon, IDWriteTextFormat* fmt, const D2D1_RECT_F& rect, BrushId brush_id, float alpha, D2D1_SIZE_F box)
{
    if (!fmt) {
        return;
    }
    auto it = std::ranges::find_if(icon_layouts_, [&](const CenteredTextLayout& s) { return s.format == fmt && s.text == icon; });
    if (it == icon_layouts_.end()) {
        it = icon_layouts_.emplace(icon_layouts_.end());
    }
    DrawCenteredText(*it, icon, fmt, rect, brush_id, alpha, box);
}
