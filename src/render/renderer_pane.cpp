#include "renderer.h"
#include "d2d_util.h"
#include "i18n.h"
#include "pane_layout.h"
#include "string_convert.h"
#include "ui_constants.h"
#include <algorithm>
#include <concepts>

// 戻り値: キャッシュが使用可能なら true。
static bool EnsurePaneCacheSize(PaneCache& cache, ID2D1RenderTarget* parent, float width, float height)
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
        cache.Invalidate();
    }
    return true;
}

// right_x はペイン右端。つまみはそこから VScrollbarLeftX で内側に寄せる。
static void FillVScrollThumb(ID2D1RenderTarget* rt, ID2D1SolidColorBrush* brush, float right_x, float thumb_y, float thumb_height)
{
    const float x = VScrollbarLeftX(right_x);
    constexpr float radius = PANE_SCROLLBAR_WIDTH / 2.0f;
    const D2D1_ROUNDED_RECT thumb_rect{
        D2D1::RectF(x, thumb_y, x + PANE_SCROLLBAR_WIDTH, thumb_y + thumb_height), radius, radius
    };
    rt->FillRoundedRectangle(thumb_rect, brush);
}

static void DrawPaneScrollbar(
    ID2D1RenderTarget* rt, ID2D1SolidColorBrush* thumb_brush,
    float pane_width, float content_top, float content_height,
    float scroll_y, float total_content_height)
{
    if (total_content_height <= content_height) {
        return;
    }

    // ドラッグ処理 (ScrollFromThumbY) と同じ計算を共有し、見た目と挙動のズレを防ぐ。
    // bitmap RT ローカル座標 (origin 0,0) なので y=0 の矩形で組む。
    const PaneRect local_rect{ .x = 0.0f, .y = 0.0f, .width = pane_width, .height = content_top + content_height };
    const PaneScrollInfo info = ComputeScrollInfo(local_rect, content_top, total_content_height);
    if (info.thumb_height >= info.content_height) {
        return;
    }
    FillVScrollThumb(rt, thumb_brush, pane_width, ComputeThumbY(info, scroll_y), info.thumb_height);
}

// draw_item は template 経由のため別引数。
struct SidePaneDrawContext {
    PaneCache& cache;
    ID2D1RenderTarget* main_rt;
    const PaneRect& rect;
    const ScrollState& scroll;
    int item_count;
    std::wstring_view header_text;
    const Theme& theme;
    ID2D1SolidColorBrush* splitter_brush;
    ID2D1SolidColorBrush* text_brush;
    ID2D1SolidColorBrush* scrollbar_thumb_brush;
    IDWriteTextFormat* fmt_header;
    IDWriteTextFormat* fmt_close_icon;
    ID2D1SolidColorBrush* close_hover_brush;
    PaneHeaderButton hovered_button;
    // 以下はファイルペインのみ。
    bool show_file_buttons;
    bool reveal_enabled;
};

static void DrawSidePaneHeader(ID2D1RenderTarget* rt, const SidePaneDrawContext& sp)
{
    const float width = sp.rect.width;
    const float header_h = sp.theme.pane_header_height;
    rt->FillRectangle(D2D1::RectF(0, 0, width, header_h), sp.splitter_brush);

    // 無効なボタンはホバー状態が残っていても強調しない (ヘルプへ切替直後など)。
    const auto draw_button = [&](const D2D1_RECT_F& rect, const wchar_t* icon, PaneHeaderButton button, bool enabled = true) {
        if (enabled && sp.hovered_button == button) {
            rt->FillRectangle(rect, sp.close_hover_brush);
        }
        if (sp.fmt_close_icon) {
            mendo::OpacityScope dim{ enabled ? nullptr : sp.text_brush, DISABLED_UI_ALPHA };
            rt->DrawText(icon, 1, sp.fmt_close_icon, rect, sp.text_brush, D2D1_DRAW_TEXT_OPTIONS_CLIP);
        }
    };

    const D2D1_RECT_F close_rect = PaneCloseButtonRect(width, header_h);
    draw_button(close_rect, L"\uE8BB", PaneHeaderButton::Close);
    if (sp.show_file_buttons) {
        draw_button(PaneRefreshButtonRect(width, header_h), L"\uE72C", PaneHeaderButton::Refresh);
        const D2D1_RECT_F reveal_rect = PaneRevealButtonRect(width, header_h);
        draw_button(reveal_rect, L"\uE81D", PaneHeaderButton::Reveal, sp.reveal_enabled);
    }

    const auto text_rect = PaneHeaderTextRect(width, header_h, sp.show_file_buttons);
    if (!sp.fmt_header || !text_rect) {
        return;
    }
    rt->DrawText(
        sp.header_text.data(),
        static_cast<UINT32>(sp.header_text.size()),
        sp.fmt_header,
        *text_rect,
        sp.text_brush,
        D2D1_DRAW_TEXT_OPTIONS_CLIP);
}

// オフスクリーン RT へペイン全体を描き直す。失敗時はキャッシュを破棄する (cached_bitmap も null になる)。
template <typename DrawItemFn>
    requires std::invocable<DrawItemFn&, ID2D1RenderTarget*, int, float, float>
static void RedrawSidePaneCache(const SidePaneDrawContext& sp, DrawItemFn& draw_item)
{
    auto* rt = sp.cache.bitmap_rt.Get();
    rt->BeginDraw();
    rt->Clear(sp.theme.pane_bg_color);

    DrawSidePaneHeader(rt, sp);

    const float item_h = sp.theme.pane_item_height;
    const float content_top = sp.theme.pane_header_height;
    const float content_height = sp.rect.height - content_top;
    const D2D1_RECT_F clip = D2D1::RectF(0, content_top, sp.rect.width, sp.rect.height);
    rt->PushAxisAlignedClip(clip, D2D1_ANTIALIAS_MODE_ALIASED);
    rt->SetTransform(D2D1::Matrix3x2F::Translation(0, -sp.scroll.scroll_y));

    const int first = std::max(0, static_cast<int>(sp.scroll.scroll_y / item_h));
    const int last = std::min(sp.item_count - 1, static_cast<int>((sp.scroll.scroll_y + content_height) / item_h) + 1);
    for (int i = first; i <= last; i++) {
        draw_item(rt, i, content_top + i * item_h, sp.rect.width);
    }

    rt->SetTransform(D2D1::Matrix3x2F::Identity());
    rt->PopAxisAlignedClip();

    const float total_content = SidePaneContentHeight(static_cast<size_t>(sp.item_count), item_h);
    DrawPaneScrollbar(rt, sp.scrollbar_thumb_brush, sp.rect.width, content_top, content_height, sp.scroll.scroll_y, total_content);

    if (FAILED(rt->EndDraw())) {
        sp.cache.Reset();
        return;
    }
    sp.cache.cached_bitmap.Reset();
    if (FAILED(sp.cache.bitmap_rt->GetBitmap(&sp.cache.cached_bitmap))) {
        sp.cache.Reset();
        return;
    }
    sp.cache.dirty = false;
    sp.cache.cached_scroll_y = sp.scroll.scroll_y;
}

template <typename DrawItemFn>
    requires std::invocable<DrawItemFn&, ID2D1RenderTarget*, int, float, float>
static void DrawSidePaneImpl(const SidePaneDrawContext& sp, DrawItemFn draw_item)
{
    if (!EnsurePaneCacheSize(sp.cache, sp.main_rt, sp.rect.width, sp.rect.height)) {
        return;
    }
    if (sp.cache.NeedsRedraw(sp.scroll.scroll_y)) {
        RedrawSidePaneCache(sp, draw_item);
    }
    if (sp.cache.cached_bitmap) {
        sp.main_rt->DrawBitmap(sp.cache.cached_bitmap.Get(), ToD2DRect(sp.rect));
    }
}

static const wchar_t* FileEntryIcon(const FileEntry& entry) noexcept
{
    if (entry.is_parent()) {
        return L"\uE74A";
    }
    if (entry.is_directory()) {
        return L"\uE8B7";
    }
    return L"\uE8A5";
}

SidePaneDrawContext Renderer::MakeSidePaneContext(PaneTarget target, const SidePaneInstance& pane, size_t item_count, std::wstring_view header_text)
{
    const bool is_file_pane = (target == PaneTarget::File);
    return SidePaneDrawContext{
        .cache = SidePaneCache(target),
        .main_rt = rt(),
        .rect = pane.rect,
        .scroll = pane.scroll,
        .item_count = static_cast<int>(item_count),
        .header_text = header_text,
        .theme = theme_,
        .splitter_brush = Brush(BrushId::Splitter),
        .text_brush = Brush(BrushId::Text),
        .scrollbar_thumb_brush = Brush(BrushId::ScrollbarThumb),
        .fmt_header = fmt_.pane_header.Get(),
        .fmt_close_icon = fmt_.pane_icon.Get(),
        .close_hover_brush = Brush(BrushId::PaneItemHover),
        .hovered_button = pane.hovered_button,
        .show_file_buttons = is_file_pane,
        .reveal_enabled = is_file_pane && pane.reveal_enabled,
    };
}

void Renderer::DrawFileExplorer(const std::pmr::vector<FileEntry>& entries, const SidePaneInstance& pane)
{
    constexpr float icon_col_width = 24.0f;
    auto draw_item = [&](ID2D1RenderTarget* rt, int i, float item_y, float width) {
        const auto& entry = entries[i];

        const D2D1_RECT_F item_rect = D2D1::RectF(0, item_y, width, item_y + theme_.pane_item_height);
        if (entry.is_current()) {
            rt->FillRectangle(item_rect, Brush(BrushId::PaneItemActive));
        }
        else if (i == pane.hovered_index) {
            rt->FillRectangle(item_rect, Brush(BrushId::PaneItemHover));
        }

        if (fmt_.pane_icon) {
            const D2D1_RECT_F icon_rect = D2D1::RectF(4.0f, item_y, 4.0f + icon_col_width, item_y + theme_.pane_item_height);
            rt->DrawText(FileEntryIcon(entry), 1, fmt_.pane_icon.Get(), icon_rect, Brush(BrushId::Text), D2D1_DRAW_TEXT_OPTIONS_CLIP);
        }

        if (fmt_.pane_item) {
            const D2D1_RECT_F text_rect = D2D1::RectF(4.0f + icon_col_width, item_y, width - 4.0f, item_y + theme_.pane_item_height);
            const std::wstring_view name = entry.GetDisplayName();
            rt->DrawText(
                name.data(),
                static_cast<UINT32>(name.size()),
                fmt_.pane_item.Get(),
                text_rect,
                Brush(BrushId::Text),
                D2D1_DRAW_TEXT_OPTIONS_CLIP);
        }
    };
    DrawSidePaneImpl(MakeSidePaneContext(PaneTarget::File, pane, entries.size(), i18n::S().pane_header_files), draw_item);
}

void Renderer::DrawToc(const std::pmr::vector<TocEntry>& entries, const std::pmr::vector<Node>& nodes,
                       const SidePaneInstance& pane, int active_index)
{
    auto draw_item = [&](ID2D1RenderTarget* rt, int i, float item_y, float width) {
        const auto& entry = entries[i];

        if (i == active_index || i == pane.hovered_index) {
            const D2D1_RECT_F item_rect = D2D1::RectF(0, item_y, width, item_y + theme_.pane_item_height);
            const auto bid = (i == active_index) ? BrushId::PaneItemActive : BrushId::PaneItemHover;
            rt->FillRectangle(item_rect, Brush(bid));
        }

        const float text_left = 8.0f + (entry.heading_level - 1) * TOC_INDENT_PER_LEVEL;
        if (fmt_.pane_item) {
            const D2D1_RECT_F text_rect = D2D1::RectF(text_left, item_y, width - 4.0f, item_y + theme_.pane_item_height);
            string_convert::Utf8ToWide(nodes[entry.node_index].GetText(), toc_text_scratch_);
            rt->DrawText(toc_text_scratch_.data(), static_cast<UINT32>(toc_text_scratch_.size()), fmt_.pane_item.Get(), text_rect, Brush(BrushId::Text), D2D1_DRAW_TEXT_OPTIONS_CLIP);
        }

        if (i == active_index) {
            const float line_y = item_y + theme_.pane_item_height - 1.0f;
            rt->DrawLine(
                D2D1::Point2F(text_left, line_y),
                D2D1::Point2F(width - 4.0f, line_y),
                Brush(BrushId::Text),
                1.5f);
        }
    };
    DrawSidePaneImpl(MakeSidePaneContext(PaneTarget::Toc, pane, entries.size(), i18n::S().pane_header_toc), draw_item);
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
    const bool needs_scrollbar = has_dirty_nodes ? (total_content_height >= viewport_h) : (total_content_height > viewport_h);
    if (!needs_scrollbar) {
        return;
    }

    const auto info = ComputeScrollInfo(md_pane_rect, 0.0f, total_content_height);
    FillVScrollThumb(rt(), Brush(BrushId::ScrollbarThumb), md_pane_rect.x + md_pane_rect.width, ComputeThumbY(info, scroll_y), info.thumb_height);
}
