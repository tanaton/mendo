#include "renderer.h"
#include "d2d_util.h"

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

    auto drawButton = [&](const DipRect& dip_rect, const wchar_t* icon, bool show_bg, BrushId bg_id, BrushId text_id, float alpha) {
        const D2D1_RECT_F rect = ToD2DRect(dip_rect);
        if (show_bg) {
            rt()->FillRectangle(rect, Brush(bg_id));
        }
        if (fmt_.titlebar_icon) {
            auto* brush = Brush(text_id);
            if (brush) {
                mendo::OpacityScope guard{ brush, alpha };
                rt()->DrawText(icon, 1, fmt_.titlebar_icon.Get(), rect, brush);
            }
        }
    };

    const auto draw_plain = [&](const DipRect& rect, const wchar_t* icon, TitleBarHitZone zone) {
        drawButton(rect, icon, is_hovered(zone), BrushId::TitleBarButtonHover, BrushId::TitleBarText, text_alpha);
    };
    // active > hover の優先度
    const auto draw_toggle = [&](const DipRect& rect, const wchar_t* icon, bool active, TitleBarHitZone zone) {
        drawButton(
            rect,
            icon,
            active || is_hovered(zone),
            active ? BrushId::TitleBarButtonActive : BrushId::TitleBarButtonHover,
            BrushId::TitleBarText,
            text_alpha);
    };

    draw_plain(tb.open_file.rect, L"\uE838", TitleBarHitZone::OpenFile);
    draw_plain(tb.help.rect, L"\uE897", TitleBarHitZone::Help);
    draw_plain(tb.theme_toggle.rect, tb.is_dark_mode ? L"\uE706" : L"\uE708", TitleBarHitZone::ThemeToggle);
    draw_toggle(tb.search.rect, L"\uE721", tb.search_active, TitleBarHitZone::Search);
    draw_toggle(tb.file_toggle.rect, L"\uE8B7", tb.file_pane_visible, TitleBarHitZone::FileToggle);
    draw_toggle(tb.toc_toggle.rect, L"\uE8FD", tb.toc_pane_visible, TitleBarHitZone::TocToggle);
    draw_plain(tb.minimize.rect, L"\uE921", TitleBarHitZone::Minimize);
    const wchar_t max_icon[]{ tb.is_maximized ? L'\uE923' : L'\uE922', L'\0' };
    draw_plain(tb.maximize.rect, max_icon, TitleBarHitZone::Maximize);
    if (is_hovered(TitleBarHitZone::Close)) {
        drawButton(tb.close.rect, L"\uE8BB", true, BrushId::TitleBarCloseRed, BrushId::TitleBarCloseWhite, 1.0f);
    }
    else {
        draw_plain(tb.close.rect, L"\uE8BB", TitleBarHitZone::Close);
    }

    if (app_icon_bitmap_) {
        const float icon_alpha = tb.window_active ? 1.0f : 0.5f;
        rt()->DrawBitmap(app_icon_bitmap_.Get(), ToD2DRect(tb.icon_rect), icon_alpha, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
    }

    if (fmt_.titlebar_text && !tb.title_text.empty()) {
        auto* brush = Brush(BrushId::TitleBarText);
        if (brush) {
            mendo::OpacityScope guard{ brush, text_alpha };
            rt()->DrawText(
                tb.title_text.data(),
                static_cast<UINT32>(tb.title_text.size()),
                fmt_.titlebar_text.Get(),
                ToD2DRect(tb.title_text_rect),
                brush);
        }
    }
}
