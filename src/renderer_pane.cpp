#include "renderer.h"
#include <algorithm>
#include <cmath>

// Ensure a PaneCache's bitmap render target matches the required size.
// Returns true if the cache is ready to use.
static bool EnsurePaneCacheSize(Renderer::PaneCache& cache, ID2D1RenderTarget* parent,
                                float width, float height) {
    if (width <= 0 || height <= 0) return false;

    if (!cache.bitmap_rt ||
        cache.cached_width != width || cache.cached_height != height) {
        cache.bitmap_rt.Reset();
        HRESULT hr = parent->CreateCompatibleRenderTarget(
            D2D1::SizeF(width, height), &cache.bitmap_rt);
        if (FAILED(hr)) return false;
        cache.cached_width = width;
        cache.cached_height = height;
        cache.dirty = true;
    }
    return true;
}

void Renderer::DrawFileExplorer(const std::vector<FileEntry>& entries, const PaneRect& rect,
                                const ScrollState& scroll, int hovered_index) {
    if (!EnsurePaneCacheSize(file_pane_cache_, render_target_.Get(), rect.width, rect.height))
        return;

    if (file_pane_cache_.dirty) {
        auto* rt = file_pane_cache_.bitmap_rt.Get();
        rt->BeginDraw();
        rt->Clear(theme_.pane_bg_color);

        // Header background
        D2D1_RECT_F header_bg = D2D1::RectF(0, 0, rect.width, theme_.pane_header_height);
        rt->FillRectangle(header_bg, splitter_brush_.Get());
        if (fmt_pane_header_) {
            D2D1_RECT_F header_text = D2D1::RectF(8.0f, 0, rect.width - 4.0f, theme_.pane_header_height);
            rt->DrawText(L"Files", 5, fmt_pane_header_.Get(), header_text, text_brush_.Get(),
                         D2D1_DRAW_TEXT_OPTIONS_CLIP);
        }

        // Content area with clipping
        float content_top = theme_.pane_header_height;
        float content_height = rect.height - content_top;
        D2D1_RECT_F clip = D2D1::RectF(0, content_top, rect.width, rect.height);
        rt->PushAxisAlignedClip(clip, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
        rt->SetTransform(D2D1::Matrix3x2F::Translation(0, -scroll.scroll_y));

        // Viewport culling: only draw visible items
        int first = std::max(0, static_cast<int>(scroll.scroll_y / theme_.pane_item_height));
        int last = std::min(static_cast<int>(entries.size()) - 1,
            static_cast<int>((scroll.scroll_y + content_height) / theme_.pane_item_height) + 1);

        // Icon width reserved for the Segoe Fluent Icons glyph
        constexpr float icon_col_width = 24.0f;

        for (int i = first; i <= last; i++) {
            float item_y = content_top + i * theme_.pane_item_height;
            const auto& entry = entries[i];

            D2D1_RECT_F item_rect = D2D1::RectF(0, item_y, rect.width, item_y + theme_.pane_item_height);
            if (entry.is_current) {
                rt->FillRectangle(item_rect, pane_item_active_brush_.Get());
            } else if (i == hovered_index) {
                rt->FillRectangle(item_rect, pane_item_hover_brush_.Get());
            }

            // Draw icon (Segoe Fluent Icons)
            if (fmt_pane_icon_) {
                // Choose icon: folder=\uE8B7, parent(..)=\uE74A (ChevronUp), md file=\uE8A5 (Page)
                const wchar_t* icon;
                if (entry.is_parent) {
                    icon = L"\uE74A";  // ChevronUp
                } else if (entry.is_directory) {
                    icon = L"\uE8B7";  // Folder
                } else {
                    icon = L"\uE8A5";  // Page
                }
                D2D1_RECT_F icon_rect = D2D1::RectF(
                    4.0f, item_y, 4.0f + icon_col_width, item_y + theme_.pane_item_height);
                rt->DrawText(icon, 1, fmt_pane_icon_.Get(), icon_rect, text_brush_.Get(),
                             D2D1_DRAW_TEXT_OPTIONS_CLIP);
            }

            if (fmt_pane_item_) {
                D2D1_RECT_F text_rect = D2D1::RectF(
                    4.0f + icon_col_width, item_y, rect.width - 4.0f, item_y + theme_.pane_item_height);
                rt->DrawText(entry.filename.c_str(), static_cast<UINT32>(entry.filename.size()),
                             fmt_pane_item_.Get(), text_rect, text_brush_.Get(),
                             D2D1_DRAW_TEXT_OPTIONS_CLIP);
            }
        }

        rt->SetTransform(D2D1::Matrix3x2F::Identity());
        rt->PopAxisAlignedClip();

        // Draw scrollbar overlay (fixed position, not scrolled)
        float total_content = static_cast<float>(entries.size()) * theme_.pane_item_height;
        DrawPaneScrollbar(rt, rect.width, content_top, content_height,
                          scroll.scroll_y, total_content);

        rt->EndDraw();
        file_pane_cache_.dirty = false;
    }

    // Blit cached bitmap
    ComPtr<ID2D1Bitmap> bmp;
    file_pane_cache_.bitmap_rt->GetBitmap(&bmp);
    if (bmp) {
        D2D1_RECT_F dest = D2D1::RectF(rect.x, rect.y,
                                         rect.x + rect.width, rect.y + rect.height);
        render_target_->DrawBitmap(bmp.Get(), dest);
    }
}

void Renderer::DrawToc(const std::vector<TocEntry>& entries, const PaneRect& rect,
                       const ScrollState& scroll, int hovered_index) {
    if (!EnsurePaneCacheSize(toc_pane_cache_, render_target_.Get(), rect.width, rect.height))
        return;

    if (toc_pane_cache_.dirty) {
        auto* rt = toc_pane_cache_.bitmap_rt.Get();
        rt->BeginDraw();
        rt->Clear(theme_.pane_bg_color);

        // Header background
        D2D1_RECT_F header_bg = D2D1::RectF(0, 0, rect.width, theme_.pane_header_height);
        rt->FillRectangle(header_bg, splitter_brush_.Get());
        if (fmt_pane_header_) {
            D2D1_RECT_F header_text = D2D1::RectF(8.0f, 0, rect.width - 4.0f, theme_.pane_header_height);
            rt->DrawText(L"Table of Contents", 17, fmt_pane_header_.Get(), header_text,
                         text_brush_.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
        }

        // Content area with clipping
        float content_top = theme_.pane_header_height;
        float content_height = rect.height - content_top;
        D2D1_RECT_F clip = D2D1::RectF(0, content_top, rect.width, rect.height);
        rt->PushAxisAlignedClip(clip, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
        rt->SetTransform(D2D1::Matrix3x2F::Translation(0, -scroll.scroll_y));

        // Viewport culling
        int first = std::max(0, static_cast<int>(scroll.scroll_y / theme_.pane_item_height));
        int last = std::min(static_cast<int>(entries.size()) - 1,
            static_cast<int>((scroll.scroll_y + content_height) / theme_.pane_item_height) + 1);

        for (int i = first; i <= last; i++) {
            float item_y = content_top + i * theme_.pane_item_height;
            const auto& entry = entries[i];

            if (i == hovered_index) {
                D2D1_RECT_F item_rect = D2D1::RectF(0, item_y, rect.width, item_y + theme_.pane_item_height);
                rt->FillRectangle(item_rect, pane_item_hover_brush_.Get());
            }

            float indent = (entry.heading_level - 1) * 12.0f;
            if (fmt_pane_item_) {
                D2D1_RECT_F text_rect = D2D1::RectF(
                    8.0f + indent, item_y, rect.width - 4.0f, item_y + theme_.pane_item_height);
                rt->DrawText(entry.text.c_str(), static_cast<UINT32>(entry.text.size()),
                             fmt_pane_item_.Get(), text_rect, text_brush_.Get(),
                             D2D1_DRAW_TEXT_OPTIONS_CLIP);
            }
        }

        rt->SetTransform(D2D1::Matrix3x2F::Identity());
        rt->PopAxisAlignedClip();

        // Draw scrollbar overlay (fixed position, not scrolled)
        float total_content = static_cast<float>(entries.size()) * theme_.pane_item_height;
        DrawPaneScrollbar(rt, rect.width, content_top, content_height,
                          scroll.scroll_y, total_content);

        rt->EndDraw();
        toc_pane_cache_.dirty = false;
    }

    // Blit cached bitmap
    ComPtr<ID2D1Bitmap> bmp;
    toc_pane_cache_.bitmap_rt->GetBitmap(&bmp);
    if (bmp) {
        D2D1_RECT_F dest = D2D1::RectF(rect.x, rect.y,
                                         rect.x + rect.width, rect.y + rect.height);
        render_target_->DrawBitmap(bmp.Get(), dest);
    }
}

void Renderer::DrawSplitter(float x, float height) {
    D2D1_RECT_F rect = D2D1::RectF(x, 0.0f, x + theme_.splitter_width, height);
    render_target_->FillRectangle(rect, splitter_brush_.Get());
}

void Renderer::DrawPaneScrollbar(ID2D1RenderTarget* rt, float pane_width,
                                  float content_top, float content_height,
                                  float scroll_y, float total_content_height) {
    if (total_content_height <= content_height) return;

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

    rt->FillRoundedRectangle(thumb_rect, scrollbar_thumb_brush_.Get());
}
