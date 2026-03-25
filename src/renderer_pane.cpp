#include "renderer.h"
#include "pane_layout.h"
#include "ui_constants.h"
#include <algorithm>
#include <cmath>

// PaneCacheのビットマップレンダーターゲットが必要なサイズと一致することを確認。
// キャッシュが使用可能ならtrueを返す。
static bool EnsurePaneCacheSize(Renderer::PaneCache& cache, ID2D1RenderTarget* parent,
    float width, float height) {
    if (width <= 0 || height <= 0) {
        return false;
    }

    if (!cache.bitmap_rt ||
        cache.cached_width != width || cache.cached_height != height) {
        cache.bitmap_rt.Reset();
        cache.cached_bitmap.Reset();
        HRESULT hr = parent->CreateCompatibleRenderTarget(
            D2D1::SizeF(width, height), &cache.bitmap_rt);
        if (FAILED(hr)) {
            return false;
        }
        cache.cached_width = width;
        cache.cached_height = height;
        cache.dirty = true;
    }
    return true;
}

static void DrawPaneScrollbar(ID2D1RenderTarget* rt, ID2D1SolidColorBrush* thumb_brush,
    float pane_width, float content_top, float content_height,
    float scroll_y, float total_content_height) {
    if (total_content_height <= content_height) {
        return;
    }

    float track_height = content_height;
    float thumb_ratio = content_height / total_content_height;
    float thumb_height = std::max(PANE_SCROLLBAR_THUMB_MIN, track_height * thumb_ratio);

    float scroll_ratio = scroll_y / (total_content_height - content_height);
    float thumb_y = content_top + scroll_ratio * (track_height - thumb_height);

    float thumb_x = pane_width - PANE_SCROLLBAR_WIDTH - 2.0f;

    D2D1_ROUNDED_RECT thumb_rect;
    thumb_rect.rect = D2D1::RectF(thumb_x, thumb_y,
        thumb_x + PANE_SCROLLBAR_WIDTH, thumb_y + thumb_height);
    thumb_rect.radiusX = PANE_SCROLLBAR_WIDTH / 2.0f;
    thumb_rect.radiusY = PANE_SCROLLBAR_WIDTH / 2.0f;

    rt->FillRoundedRectangle(thumb_rect, thumb_brush);
}

// サイドペイン描画の共通スキャフォールド。
// DrawItemFnのシグネチャ: void(ID2D1RenderTarget* rt, int index, float item_y, float pane_width)
template<typename DrawItemFn>
static void DrawSidePaneImpl(
    Renderer::PaneCache& cache,
    ID2D1HwndRenderTarget* main_rt,
    const PaneRect& rect,
    const ScrollState& scroll,
    int item_count,
    std::wstring_view header_text,
    const Theme& theme,
    ID2D1SolidColorBrush* splitter_brush,
    ID2D1SolidColorBrush* text_brush,
    ID2D1SolidColorBrush* scrollbar_thumb_brush,
    IDWriteTextFormat* fmt_header,
    DrawItemFn draw_item)
{
    if (!EnsurePaneCacheSize(cache, main_rt, rect.width, rect.height))
        return;

    if (cache.dirty) {
        auto* rt = cache.bitmap_rt.Get();
        rt->BeginDraw();
        rt->Clear(theme.pane_bg_color);

        // ヘッダー
        D2D1_RECT_F header_bg = D2D1::RectF(0, 0, rect.width, theme.pane_header_height);
        rt->FillRectangle(header_bg, splitter_brush);
        if (fmt_header) {
            D2D1_RECT_F header_rect = D2D1::RectF(8.0f, 0, rect.width - 4.0f, theme.pane_header_height);
            rt->DrawText(header_text.data(), static_cast<UINT32>(header_text.size()),
                fmt_header, header_rect, text_brush, D2D1_DRAW_TEXT_OPTIONS_CLIP);
        }

        // クリッピング付きコンテンツ領域
        float content_top = theme.pane_header_height;
        float content_height = rect.height - content_top;
        D2D1_RECT_F clip = D2D1::RectF(0, content_top, rect.width, rect.height);
        rt->PushAxisAlignedClip(clip, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
        rt->SetTransform(D2D1::Matrix3x2F::Translation(0, -scroll.scroll_y));

        // ビューポートカリング
        int first = std::max(0, static_cast<int>(scroll.scroll_y / theme.pane_item_height));
        int last = std::min(item_count - 1,
            static_cast<int>((scroll.scroll_y + content_height) / theme.pane_item_height) + 1);

        for (int i = first; i <= last; i++) {
            float item_y = content_top + i * theme.pane_item_height;
            draw_item(rt, i, item_y, rect.width);
        }

        rt->SetTransform(D2D1::Matrix3x2F::Identity());
        rt->PopAxisAlignedClip();

        // スクロールバーオーバーレイ
        float total_content = static_cast<float>(item_count) * theme.pane_item_height;
        DrawPaneScrollbar(rt, scrollbar_thumb_brush,
            rect.width, content_top, content_height,
            scroll.scroll_y, total_content);

        rt->EndDraw();
        cache.dirty = false;
        cache.cached_bitmap.Reset();
        cache.bitmap_rt->GetBitmap(&cache.cached_bitmap);
    }

    // キャッシュされたビットマップを転送
    if (cache.cached_bitmap) {
        D2D1_RECT_F dest = D2D1::RectF(rect.x, rect.y,
            rect.x + rect.width, rect.y + rect.height);
        main_rt->DrawBitmap(cache.cached_bitmap.Get(), dest);
    }
}

void Renderer::DrawFileExplorer(const std::pmr::vector<FileEntry>& entries, const PaneRect& rect,
    const ScrollState& scroll, int hovered_index) {
    constexpr float icon_col_width = 24.0f;

    DrawSidePaneImpl(file_pane_cache_, rt(), rect, scroll,
        static_cast<int>(entries.size()), L"Files", theme_,
        Brush(BrushId::Splitter), Brush(BrushId::Text),
        Brush(BrushId::ScrollbarThumb), fmt_pane_header_.Get(),
        [&](ID2D1RenderTarget* rt, int i, float item_y, float width) {
        const auto& entry = entries[i];

        D2D1_RECT_F item_rect = D2D1::RectF(0, item_y, width, item_y + theme_.pane_item_height);
        if (entry.is_current) {
            rt->FillRectangle(item_rect, Brush(BrushId::PaneItemActive));
        }
        else if (i == hovered_index) {
            rt->FillRectangle(item_rect, Brush(BrushId::PaneItemHover));
        }

        if (fmt_pane_icon_) {
            const wchar_t* icon;
            if (entry.is_parent) icon = L"\uE74A";
            else if (entry.is_directory) icon = L"\uE8B7";
            else icon = L"\uE8A5";
            D2D1_RECT_F icon_rect = D2D1::RectF(
                4.0f, item_y, 4.0f + icon_col_width, item_y + theme_.pane_item_height);
            rt->DrawText(icon, 1, fmt_pane_icon_.Get(), icon_rect, Brush(BrushId::Text),
                D2D1_DRAW_TEXT_OPTIONS_CLIP);
        }

        if (fmt_pane_item_) {
            D2D1_RECT_F text_rect = D2D1::RectF(
                4.0f + icon_col_width, item_y, width - 4.0f, item_y + theme_.pane_item_height);
            rt->DrawText(entry.filename.c_str(), static_cast<UINT32>(entry.filename.size()),
                fmt_pane_item_.Get(), text_rect, Brush(BrushId::Text),
                D2D1_DRAW_TEXT_OPTIONS_CLIP);
        }
    });
}

void Renderer::DrawToc(const std::pmr::vector<TocEntry>& entries, const PaneRect& rect,
    const ScrollState& scroll, int hovered_index) {
    DrawSidePaneImpl(toc_pane_cache_, rt(), rect, scroll,
        static_cast<int>(entries.size()), L"Table of Contents", theme_,
        Brush(BrushId::Splitter), Brush(BrushId::Text),
        Brush(BrushId::ScrollbarThumb), fmt_pane_header_.Get(),
        [&](ID2D1RenderTarget* rt, int i, float item_y, float width) {
        const auto& entry = entries[i];

        if (i == hovered_index) {
            D2D1_RECT_F item_rect = D2D1::RectF(0, item_y, width, item_y + theme_.pane_item_height);
            rt->FillRectangle(item_rect, Brush(BrushId::PaneItemHover));
        }

        float indent = (entry.heading_level - 1) * TOC_INDENT_PER_LEVEL;
        if (fmt_pane_item_) {
            D2D1_RECT_F text_rect = D2D1::RectF(
                8.0f + indent, item_y, width - 4.0f, item_y + theme_.pane_item_height);
            rt->DrawText(entry.text.c_str(), static_cast<UINT32>(entry.text.size()),
                fmt_pane_item_.Get(), text_rect, Brush(BrushId::Text),
                D2D1_DRAW_TEXT_OPTIONS_CLIP);
        }
    });
}

void Renderer::DrawSplitter(float x, float top, float bottom) {
    D2D1_RECT_F rect = D2D1::RectF(x, top, x + theme_.splitter_width, bottom);
    rt()->FillRectangle(rect, Brush(BrushId::Splitter));
}

void Renderer::DrawMdScrollbar(const PaneRect& md_pane_rect, float scroll_y, float total_content_height) {
    float viewport_h = md_pane_rect.height;
    if (total_content_height <= viewport_h || viewport_h <= 0.0f) {
        return;
    }

    auto info = ComputeScrollInfo(md_pane_rect, 0.0f, total_content_height);
    float thumb_y = ComputeThumbY(info, scroll_y);
    float track_x = md_pane_rect.x + md_pane_rect.width - PANE_SCROLLBAR_WIDTH - PANE_SCROLLBAR_MARGIN;

    D2D1_ROUNDED_RECT thumb_rect;
    thumb_rect.rect = D2D1::RectF(track_x, thumb_y, track_x + PANE_SCROLLBAR_WIDTH, thumb_y + info.thumb_height);
    thumb_rect.radiusX = PANE_SCROLLBAR_WIDTH / 2.0f;
    thumb_rect.radiusY = PANE_SCROLLBAR_WIDTH / 2.0f;
    rt()->FillRoundedRectangle(thumb_rect, Brush(BrushId::ScrollbarThumb));
}

void Renderer::DrawTitleBar(const TitleBarRenderState& tb) {
    if (tb.height <= 0.0f) {
        return;
    }

    // タイトルバー背景（完全不透明でガラス効果を隠す）
    D2D1_RECT_F bg_rect = D2D1::RectF(0.0f, 0.0f, tb.window_width, tb.height);
    rt()->FillRectangle(bg_rect, Brush(BrushId::TitleBarBg));

    float text_alpha = tb.window_active ? 1.0f : 0.5f;

    // アイコンボタン描画ヘルパー
    auto drawButton = [&](const D2D1_RECT_F& rect, const wchar_t* icon,
                          bool show_bg, BrushId bg_id, BrushId text_id, float alpha) {
        if (show_bg) {
            rt()->FillRectangle(rect, Brush(bg_id));
        }
        if (fmt_titlebar_icon_) {
            auto* brush = Brush(text_id);
            if (brush) {
                brush->SetOpacity(alpha);
                rt()->DrawText(icon, 1, fmt_titlebar_icon_.Get(), rect, brush);
                brush->SetOpacity(1.0f);
            }
        }
    };

    // ペイン切替ボタン（active > hover の優先度）
    drawButton(tb.file_btn_rect, L"\uE8B7",
        tb.file_pane_visible || tb.file_btn_hovered,
        tb.file_pane_visible ? BrushId::TitleBarButtonActive : BrushId::TitleBarButtonHover,
        BrushId::TitleBarText, text_alpha);

    drawButton(tb.toc_btn_rect, L"\uE8FD",
        tb.toc_pane_visible || tb.toc_btn_hovered,
        tb.toc_pane_visible ? BrushId::TitleBarButtonActive : BrushId::TitleBarButtonHover,
        BrushId::TitleBarText, text_alpha);

    // キャプションボタン
    drawButton(tb.minimize_btn_rect, L"\uE921",
        tb.minimize_btn_hovered, BrushId::TitleBarButtonHover,
        BrushId::TitleBarText, text_alpha);

    const wchar_t max_icon[] = { tb.is_maximized ? L'\uE923' : L'\uE922', L'\0' };
    drawButton(tb.maximize_btn_rect, max_icon,
        tb.maximize_btn_hovered, BrushId::TitleBarButtonHover,
        BrushId::TitleBarText, text_alpha);

    // 閉じるボタン（ホバー時は赤背景＋白アイコン）
    if (tb.close_btn_hovered) {
        drawButton(tb.close_btn_rect, L"\uE8BB",
            true, BrushId::TitleBarCloseRed,
            BrushId::TitleBarCloseWhite, 1.0f);
    } else {
        drawButton(tb.close_btn_rect, L"\uE8BB",
            false, BrushId::TitleBarButtonHover,
            BrushId::TitleBarText, text_alpha);
    }

    // タイトルテキスト
    if (fmt_titlebar_text_ && !tb.title_text.empty()) {
        auto* brush = Brush(BrushId::TitleBarText);
        if (brush) {
            brush->SetOpacity(text_alpha);
            rt()->DrawText(
                tb.title_text.data(),
                static_cast<UINT32>(tb.title_text.size()),
                fmt_titlebar_text_.Get(),
                tb.title_text_rect,
                brush);
            brush->SetOpacity(1.0f);
        }
    }
}

