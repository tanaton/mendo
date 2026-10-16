#include "renderer.h"
#include "d2d_util.h"
#include "i18n.h"
#include "pane_layout.h"
#include "string_convert.h"
#include "ui_constants.h"
#include <algorithm>
#include <concepts>

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
    // ペイン原点へ平行移動した座標系で描くので y=0 の矩形で組む。
    const PaneRect local_rect{ .x = 0.0f, .y = 0.0f, .width = pane_width, .height = content_top + content_height };
    const PaneScrollInfo info = ComputeScrollInfo(local_rect, content_top, total_content_height);
    if (info.thumb_height >= info.content_height) {
        return;
    }
    FillVScrollThumb(rt, thumb_brush, pane_width, ComputeThumbY(info, scroll_y), info.thumb_height);
}

// 中央寄せのアイコン用。箱の大きさは固定なのでレイアウトは使い回される。
static void DrawCachedText(ID2D1RenderTarget* rt, TextLayoutCache& layouts, IDWriteTextFormat* fmt,
                           std::wstring_view text, const D2D1_RECT_F& rect, ID2D1Brush* brush)
{
    const auto box = D2D1::SizeF(rect.right - rect.left, rect.bottom - rect.top);
    layouts.Draw(rt, fmt, text, rect, brush, box, D2D1_DRAW_TEXT_OPTIONS_CLIP);
}

// NO_WRAP で左寄せのテキストは箱の幅で見た目が変わらないので、幅を固定してスプリッタのドラッグ中も
// レイアウトを使い回す。省略記号は付かないため、はみ出す分は rect で切る。
static void DrawClippedText(ID2D1RenderTarget* rt, TextLayoutCache& layouts, IDWriteTextFormat* fmt,
                            std::wstring_view text, const D2D1_RECT_F& rect, ID2D1Brush* brush)
{
    constexpr float kLayoutWidth = 4096.0f;
    rt->PushAxisAlignedClip(rect, D2D1_ANTIALIAS_MODE_ALIASED);
    layouts.Draw(rt, fmt, text, rect, brush, D2D1::SizeF(kLayoutWidth, rect.bottom - rect.top));
    rt->PopAxisAlignedClip();
}

// draw_item は template 経由のため別引数。
struct SidePaneDrawContext {
    TextLayoutCache& text_layouts;
    ID2D1RenderTarget* main_rt;
    const PaneRect& rect;
    const ScrollState& scroll;
    int item_count;
    std::wstring_view header_text;
    const Theme& theme;
    ID2D1SolidColorBrush* bg_brush;
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
    const auto draw_button = [&](const D2D1_RECT_F& rect, std::wstring_view icon, PaneHeaderButton button, bool enabled = true) {
        if (enabled && sp.hovered_button == button) {
            rt->FillRectangle(rect, sp.close_hover_brush);
        }
        OpacityScope dim{ enabled ? nullptr : sp.text_brush, DISABLED_UI_ALPHA };
        DrawCachedText(rt, sp.text_layouts, sp.fmt_close_icon, icon, rect, sp.text_brush);
    };

    const D2D1_RECT_F close_rect = PaneCloseButtonRect(width, header_h);
    draw_button(close_rect, L"\uE8BB", PaneHeaderButton::Close);
    if (sp.show_file_buttons) {
        draw_button(PaneRefreshButtonRect(width, header_h), L"\uE72C", PaneHeaderButton::Refresh);
        const D2D1_RECT_F reveal_rect = PaneRevealButtonRect(width, header_h);
        draw_button(reveal_rect, L"\uE81D", PaneHeaderButton::Reveal, sp.reveal_enabled);
    }

    if (const auto text_rect = PaneHeaderTextRect(width, header_h, sp.show_file_buttons)) {
        DrawClippedText(rt, sp.text_layouts, sp.fmt_header, sp.header_text, *text_rect, sp.text_brush);
    }
}

// 可視項目は数十個でレイアウトも使い回すため、オフスクリーンに溜めず毎フレーム直接描く。
template <typename DrawItemFn>
    requires std::invocable<DrawItemFn&, ID2D1RenderTarget*, int, float, float>
static void DrawSidePaneImpl(const SidePaneDrawContext& sp, DrawItemFn draw_item)
{
    const float width = sp.rect.width;
    if (width <= 0 || sp.rect.height <= 0) {
        return;
    }
    auto* const rt = sp.main_rt;
    D2D1::Matrix3x2F saved;
    rt->GetTransform(&saved);
    const D2D1::Matrix3x2F origin = D2D1::Matrix3x2F::Translation(sp.rect.x, sp.rect.y) * saved;
    rt->SetTransform(origin);
    rt->FillRectangle(D2D1::RectF(0, 0, width, sp.rect.height), sp.bg_brush);

    DrawSidePaneHeader(rt, sp);

    const float item_h = sp.theme.pane_item_height;
    const float content_top = sp.theme.pane_header_height;
    const float content_height = sp.rect.height - content_top;
    rt->PushAxisAlignedClip(D2D1::RectF(0, content_top, width, sp.rect.height), D2D1_ANTIALIAS_MODE_ALIASED);
    rt->SetTransform(D2D1::Matrix3x2F::Translation(0, -sp.scroll.scroll_y) * origin);

    const int first = std::max(0, static_cast<int>(sp.scroll.scroll_y / item_h));
    const int last = std::min(sp.item_count - 1, static_cast<int>((sp.scroll.scroll_y + content_height) / item_h) + 1);
    for (int i = first; i <= last; i++) {
        draw_item(rt, i, content_top + i * item_h, width);
    }

    rt->SetTransform(origin);
    rt->PopAxisAlignedClip();

    const float total_content = SidePaneContentHeight(static_cast<size_t>(sp.item_count), item_h);
    DrawPaneScrollbar(rt, sp.scrollbar_thumb_brush, width, content_top, content_height, sp.scroll.scroll_y, total_content);
    rt->SetTransform(saved);
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
        .text_layouts = text_layouts_,
        .main_rt = rt(),
        .rect = pane.rect,
        .scroll = pane.scroll,
        .item_count = static_cast<int>(item_count),
        .header_text = header_text,
        .theme = theme_,
        .bg_brush = Brush(BrushId::PaneBg),
        .splitter_brush = Brush(BrushId::Splitter),
        .text_brush = Brush(BrushId::Text),
        .scrollbar_thumb_brush = Brush(BrushId::ScrollbarThumb),
        .fmt_header = fmt_.pane_header.Get(),
        .fmt_close_icon = fmt_.pane_icon.Get(),
        .close_hover_brush = Brush(BrushId::PaneItemHover),
        .hovered_button = pane.hovered_button,
        .show_file_buttons = is_file_pane,
        .reveal_enabled = pane.reveal_enabled,
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

        const D2D1_RECT_F icon_rect = D2D1::RectF(4.0f, item_y, 4.0f + icon_col_width, item_y + theme_.pane_item_height);
        DrawCachedText(rt, text_layouts_, fmt_.pane_icon.Get(), FileEntryIcon(entry), icon_rect, Brush(BrushId::Text));

        const D2D1_RECT_F text_rect = D2D1::RectF(4.0f + icon_col_width, item_y, width - 4.0f, item_y + theme_.pane_item_height);
        DrawClippedText(rt, text_layouts_, fmt_.pane_item.Get(), entry.GetDisplayName(), text_rect, Brush(BrushId::Text));
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
        const D2D1_RECT_F text_rect = D2D1::RectF(text_left, item_y, width - 4.0f, item_y + theme_.pane_item_height);
        string_convert::Utf8ToWide(nodes[entry.node_index].GetText(), toc_text_scratch_);
        DrawClippedText(rt, text_layouts_, fmt_.pane_item.Get(), toc_text_scratch_, text_rect, Brush(BrushId::Text));

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
