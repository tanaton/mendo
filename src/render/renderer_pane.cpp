#include "renderer.h"
#include "i18n.h"
#include "pane_layout.h"
#include "ui_constants.h"
#include <algorithm>
#include <cmath>

// PaneCacheのビットマップレンダーターゲットが必要なサイズと一致することを確認。
// キャッシュが使用可能ならtrueを返す。
static bool EnsurePaneCacheSize(PaneCache& cache, ID2D1RenderTarget* parent,
    float width, float height)
{
    if (width <= 0 || height <= 0) {
        return false;
    }

    if (!cache.bitmap_rt ||
        cache.cached_width != width || cache.cached_height != height) {
        cache.bitmap_rt.Reset();
        cache.cached_bitmap.Reset();
        const HRESULT hr = parent->CreateCompatibleRenderTarget(D2D1::SizeF(width, height), &cache.bitmap_rt);
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
    float scroll_y, float total_content_height)
{
    if (total_content_height <= content_height) {
        return;
    }

    const float track_height = content_height;
    const float thumb_ratio = content_height / total_content_height;
    const float thumb_height = std::max(PANE_SCROLLBAR_THUMB_MIN, track_height * thumb_ratio);

    const float scroll_ratio = scroll_y / (total_content_height - content_height);
    const float thumb_y = content_top + scroll_ratio * (track_height - thumb_height);

    const float thumb_x = pane_width - PANE_SCROLLBAR_WIDTH - 2.0f;

    D2D1_ROUNDED_RECT thumb_rect;
    thumb_rect.rect = D2D1::RectF(thumb_x, thumb_y, thumb_x + PANE_SCROLLBAR_WIDTH, thumb_y + thumb_height);
    thumb_rect.radiusX = PANE_SCROLLBAR_WIDTH / 2.0f;
    thumb_rect.radiusY = PANE_SCROLLBAR_WIDTH / 2.0f;

    rt->FillRoundedRectangle(thumb_rect, thumb_brush);
}

// サイドペイン描画の共通スキャフォールド。
// DrawItemFnのシグネチャ: void(ID2D1RenderTarget* rt, int index, float item_y, float pane_width)
template<typename DrawItemFn>
static void DrawSidePaneImpl(
    PaneCache& cache,
    ID2D1RenderTarget* main_rt,
    const PaneRect& rect,
    const ScrollState& scroll,
    int item_count,
    std::wstring_view header_text,
    const Theme& theme,
    ID2D1SolidColorBrush* splitter_brush,
    ID2D1SolidColorBrush* text_brush,
    ID2D1SolidColorBrush* scrollbar_thumb_brush,
    IDWriteTextFormat* fmt_header,
    IDWriteTextFormat* fmt_close_icon,
    ID2D1SolidColorBrush* close_hover_brush,
    bool close_hovered,
    bool show_refresh,
    bool refresh_hovered,
    DrawItemFn draw_item)
{
    if (!EnsurePaneCacheSize(cache, main_rt, rect.width, rect.height)) {
        return;
    }

    if (cache.dirty) {
        auto* rt = cache.bitmap_rt.Get();
        rt->BeginDraw();
        rt->Clear(theme.pane_bg_color);

        // ヘッダー
        const D2D1_RECT_F header_bg = D2D1::RectF(0, 0, rect.width, theme.pane_header_height);
        rt->FillRectangle(header_bg, splitter_brush);

        // 閉じるボタン
        const D2D1_RECT_F close_rect = PaneCloseButtonRect(rect.width, theme.pane_header_height);
        if (close_hovered) {
            rt->FillRectangle(close_rect, close_hover_brush);
        }
        if (fmt_close_icon) {
            rt->DrawText(L"\uE8BB", 1, fmt_close_icon, close_rect, text_brush, D2D1_DRAW_TEXT_OPTIONS_CLIP);
        }

        // 更新ボタン（閉じるボタンの左隣、ファイルペインのみ）
        float header_text_right = close_rect.left - 4.0f;
        if (show_refresh) {
            const D2D1_RECT_F refresh_rect = PaneRefreshButtonRect(rect.width, theme.pane_header_height);
            if (refresh_hovered) {
                rt->FillRectangle(refresh_rect, close_hover_brush);
            }
            if (fmt_close_icon) {
                rt->DrawText(L"\uE72C", 1, fmt_close_icon, refresh_rect, text_brush,
                    D2D1_DRAW_TEXT_OPTIONS_CLIP);
            }
            header_text_right = refresh_rect.left - 4.0f;
        }

        if (fmt_header) {
            const D2D1_RECT_F header_rect = D2D1::RectF(8.0f, 0, header_text_right, theme.pane_header_height);
            rt->DrawText(
                header_text.data(),
                static_cast<UINT32>(header_text.size()),
                fmt_header,
                header_rect,
                text_brush,
                D2D1_DRAW_TEXT_OPTIONS_CLIP
            );
        }

        // クリッピング付きコンテンツ領域
        const float content_top = theme.pane_header_height;
        const float content_height = rect.height - content_top;
        const D2D1_RECT_F clip = D2D1::RectF(0, content_top, rect.width, rect.height);
        rt->PushAxisAlignedClip(clip, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
        rt->SetTransform(D2D1::Matrix3x2F::Translation(0, -scroll.scroll_y));

        // ビューポートカリング
        const int first = std::max(0, static_cast<int>(scroll.scroll_y / theme.pane_item_height));
        const int last = std::min(
            item_count - 1,
            static_cast<int>((scroll.scroll_y + content_height) / theme.pane_item_height) + 1
        );

        for (int i = first; i <= last; i++) {
            const float item_y = content_top + i * theme.pane_item_height;
            draw_item(rt, i, item_y, rect.width);
        }

        rt->SetTransform(D2D1::Matrix3x2F::Identity());
        rt->PopAxisAlignedClip();

        // スクロールバーオーバーレイ
        const float total_content = static_cast<float>(item_count) * theme.pane_item_height;
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
        const D2D1_RECT_F dest = D2D1::RectF(rect.x, rect.y, rect.x + rect.width, rect.y + rect.height);
        main_rt->DrawBitmap(cache.cached_bitmap.Get(), dest);
    }
}

void Renderer::DrawFileExplorer(const std::pmr::vector<FileEntry>& entries, const PaneRect& rect,
    const ScrollState& scroll, int hovered_index, bool close_hovered, bool refresh_hovered)
{
    constexpr float icon_col_width = 24.0f;
    auto draw_item = [&](ID2D1RenderTarget* rt, int i, float item_y, float width) {
        const auto& entry = entries[i];

        const D2D1_RECT_F item_rect = D2D1::RectF(0, item_y, width, item_y + theme_.pane_item_height);
        if (entry.is_current) {
            rt->FillRectangle(item_rect, Brush(BrushId::PaneItemActive));
        }
        else if (i == hovered_index) {
            rt->FillRectangle(item_rect, Brush(BrushId::PaneItemHover));
        }

        if (fmt_.pane_icon) {
            const wchar_t* icon;
            if (entry.is_parent) {
                icon = L"\uE74A";
            }
            else if (entry.is_directory) {
                icon = L"\uE8B7";
            }
            else {
                icon = L"\uE8A5";
            }
            const D2D1_RECT_F icon_rect = D2D1::RectF(4.0f, item_y, 4.0f + icon_col_width, item_y + theme_.pane_item_height);
            rt->DrawText(icon, 1, fmt_.pane_icon.Get(), icon_rect, Brush(BrushId::Text), D2D1_DRAW_TEXT_OPTIONS_CLIP);
        }

        if (fmt_.pane_item) {
            const D2D1_RECT_F text_rect = D2D1::RectF(4.0f + icon_col_width, item_y, width - 4.0f, item_y + theme_.pane_item_height);
            const wchar_t* name = entry.GetDisplayName();
            rt->DrawText(
                name,
                static_cast<UINT32>(wcslen(name)),
                fmt_.pane_item.Get(),
                text_rect,
                Brush(BrushId::Text),
                D2D1_DRAW_TEXT_OPTIONS_CLIP
            );
        }
    };
    DrawSidePaneImpl(
        file_pane_cache_,
        rt(),
        rect,
        scroll,
        static_cast<int>(entries.size()),
        i18n::S().pane_header_files,
        theme_,
        Brush(BrushId::Splitter),
        Brush(BrushId::Text),
        Brush(BrushId::ScrollbarThumb),
        fmt_.pane_header.Get(),
        fmt_.pane_icon.Get(),
        Brush(BrushId::PaneItemHover),
        close_hovered,
        true,
        refresh_hovered,
        draw_item
    );
}

void Renderer::DrawToc(const std::pmr::vector<TocEntry>& entries, const std::pmr::vector<Node>& nodes,
    const PaneRect& rect, const ScrollState& scroll, int hovered_index, bool close_hovered, int active_index)
{
    auto draw_item = [&](ID2D1RenderTarget* rt, int i, float item_y, float width) {
        const auto& entry = entries[i];

        if (i == active_index || i == hovered_index) {
            const D2D1_RECT_F item_rect = D2D1::RectF(0, item_y, width, item_y + theme_.pane_item_height);
            const auto bid = (i == active_index) ? BrushId::PaneItemActive : BrushId::PaneItemHover;
            rt->FillRectangle(item_rect, Brush(bid));
        }

        const float indent = (entry.heading_level - 1) * TOC_INDENT_PER_LEVEL;
        if (fmt_.pane_item) {
            const D2D1_RECT_F text_rect = D2D1::RectF(
                8.0f + indent, item_y, width - 4.0f, item_y + theme_.pane_item_height);
            const auto& text = nodes[entry.node_index].GetText();
            rt->DrawText(text.c_str(), static_cast<UINT32>(text.size()),
                fmt_.pane_item.Get(), text_rect, Brush(BrushId::Text),
                D2D1_DRAW_TEXT_OPTIONS_CLIP);
        }

        // アクティブ見出しの下線
        if (i == active_index) {
            const float line_y = item_y + theme_.pane_item_height - 1.0f;
            const float line_left = 8.0f + indent;
            rt->DrawLine(
                D2D1::Point2F(line_left, line_y),
                D2D1::Point2F(width - 4.0f, line_y),
                Brush(BrushId::Text),
                1.5f
            );
        }
    };
    DrawSidePaneImpl(
        toc_pane_cache_,
        rt(),
        rect,
        scroll,
        static_cast<int>(entries.size()),
        i18n::S().pane_header_toc,
        theme_,
        Brush(BrushId::Splitter),
        Brush(BrushId::Text),
        Brush(BrushId::ScrollbarThumb),
        fmt_.pane_header.Get(),
        fmt_.pane_icon.Get(),
        Brush(BrushId::PaneItemHover),
        close_hovered,
        false,
        false,
        draw_item
    );
}

void Renderer::DrawSplitter(float x, float top, float bottom)
{
    const D2D1_RECT_F rect = D2D1::RectF(x, top, x + theme_.splitter_width, bottom);
    rt()->FillRectangle(rect, Brush(BrushId::Splitter));
}

void Renderer::DrawMdScrollbar(const PaneRect& md_pane_rect, float scroll_y, float total_content_height, bool has_dirty_nodes)
{
    const float viewport_h = md_pane_rect.height;
    if (viewport_h <= 0.0f) {
        return;
    }
    // ダーティノードがある間は高さが増える可能性があるため、ぴったり一致でもスクロールバーを表示し続ける
    const bool needs_scrollbar = has_dirty_nodes
        ? (total_content_height >= viewport_h)
        : (total_content_height > viewport_h);
    if (!needs_scrollbar) {
        return;
    }

    const auto info = ComputeScrollInfo(md_pane_rect, 0.0f, total_content_height);
    const float thumb_y = ComputeThumbY(info, scroll_y);
    const float track_x = md_pane_rect.x + md_pane_rect.width - PANE_SCROLLBAR_WIDTH - PANE_SCROLLBAR_MARGIN;

    D2D1_ROUNDED_RECT thumb_rect;
    thumb_rect.rect = D2D1::RectF(track_x, thumb_y, track_x + PANE_SCROLLBAR_WIDTH, thumb_y + info.thumb_height);
    thumb_rect.radiusX = PANE_SCROLLBAR_WIDTH / 2.0f;
    thumb_rect.radiusY = PANE_SCROLLBAR_WIDTH / 2.0f;
    rt()->FillRoundedRectangle(thumb_rect, Brush(BrushId::ScrollbarThumb));
}

