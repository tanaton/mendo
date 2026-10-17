#include "renderer.h"
#include "d2d_util.h"
#include "i18n.h"
#include "pane_layout.h"
#include "string_convert.h"
#include "ui_constants.h"
#include "utf8_codec.h"
#include <algorithm>

// right_x はペイン右端。つまみはそこから VScrollbarLeftX で内側に寄せる。
static D2D1_ROUNDED_RECT VScrollThumbRect(float right_x, float thumb_y, float thumb_height) noexcept
{
    const float x = VScrollbarLeftX(right_x);
    constexpr float radius = PANE_SCROLLBAR_WIDTH / 2.0f;
    return { D2D1::RectF(x, thumb_y, x + PANE_SCROLLBAR_WIDTH, thumb_y + thumb_height), radius, radius };
}

// 目次の行は 1 行で狭い。極端に長い見出しでも毎フレーム全文を UTF-16 化・ハッシュしないよう先頭だけ使う。
static std::string_view TocItemText(std::string_view heading) noexcept
{
    constexpr uint32_t kMaxBytes = 512;
    return heading.size() <= kMaxBytes ? heading : heading.substr(0, utf8_codec::SnapToCpStart(heading, kMaxBytes));
}

// ドラッグ処理 (ScrollFromThumbY) と同じ計算を共有し、見た目と挙動のズレを防ぐ。
static void GenPaneScrollbar(DrawCommandList& cmds, const PaneRect& pane, float header_height, float scroll_y, float total_content_height)
{
    const PaneScrollInfo info = ComputeScrollInfo(pane, header_height, total_content_height);
    if (info.thumb_height >= info.content_height) {
        return;
    }
    const auto thumb = VScrollThumbRect(pane.x + pane.width, ComputeThumbY(info, scroll_y), info.thumb_height);
    cmds.push_back(FillRoundedRectCmd{ .rect = thumb.rect, .rx = thumb.radiusX, .ry = thumb.radiusY, .brush_id = BrushId::ScrollbarThumb });
}

void Renderer::EmitLeftText(IDWriteTextFormat* fmt, std::wstring_view text, float x, float y, float height)
{
    // 箱の幅で見た目が変わらないので、幅を固定してスプリッタのドラッグ中もレイアウトを使い回す。
    constexpr float kLayoutWidth = 4096.0f;
    EmitChromeText(text, fmt, D2D1::RectF(x, y, x + kLayoutWidth, y + height), BrushId::Text, 1.0f, D2D1::SizeF(kLayoutWidth, height));
}

void Renderer::GenSidePaneHeader(const SidePaneInstance& pane, bool is_file_pane, std::wstring_view header_text)
{
    const PaneRect& r = pane.rect;
    const float header_h = theme_.pane_header_height;
    chrome_cmds_.push_back(FillRectCmd{ .rect = D2D1::RectF(r.x, r.y, r.x + r.width, r.y + header_h), .brush_id = BrushId::Splitter });

    // 無効なボタンはホバー状態が残っていても強調しない (ヘルプへ切替直後など)。
    // 箱の大きさはペイン内座標の矩形から取る。ずらした後の差では丸めでキャッシュが外れる。
    const auto gen_button = [&](const D2D1_RECT_F& local, std::wstring_view icon, PaneHeaderButton button, bool enabled = true) {
        const D2D1_RECT_F rect = OffsetRectF(local, r.x, r.y);
        if (enabled && pane.hovered_button == button) {
            chrome_cmds_.push_back(FillRectCmd{ .rect = rect, .brush_id = BrushId::PaneItemHover });
        }
        const auto box = D2D1::SizeF(local.right - local.left, local.bottom - local.top);
        EmitChromeText(icon, fmt_.pane_icon.Get(), rect, BrushId::Text, enabled ? 1.0f : DISABLED_UI_ALPHA, box);
    };

    gen_button(PaneCloseButtonRect(r.width, header_h), L"\uE8BB", PaneHeaderButton::Close);
    if (is_file_pane) {
        gen_button(PaneRefreshButtonRect(r.width, header_h), L"\uE72C", PaneHeaderButton::Refresh);
        gen_button(PaneRevealButtonRect(r.width, header_h), L"\uE81D", PaneHeaderButton::Reveal, pane.reveal_enabled);
    }

    // ボタンに重ならないよう切る。
    if (const auto text_rect = PaneHeaderTextRect(r.width, header_h, is_file_pane)) {
        const D2D1_RECT_F clip = OffsetRectF(*text_rect, r.x, r.y);
        chrome_cmds_.push_back(PushClipCmd{ clip });
        EmitLeftText(fmt_.pane_header.Get(), header_text, clip.left, clip.top, text_rect->bottom - text_rect->top);
        chrome_cmds_.push_back(PopClipCmd{});
    }
}

// 項目のはみ出しは内容領域のクリップ 1 回で切り、項目ごとの Push/Pop を避ける。
template <typename GenItemFn>
void Renderer::GenSidePane(const SidePaneInstance& pane, bool is_file_pane, size_t item_count, std::wstring_view header_text, GenItemFn gen_item)
{
    const PaneRect& r = pane.rect;
    if (r.width <= 0 || r.height <= 0) {
        return;
    }
    chrome_cmds_.push_back(FillRectCmd{ .rect = ToD2DRect(r), .brush_id = BrushId::PaneBg });

    GenSidePaneHeader(pane, is_file_pane, header_text);

    const float item_h = theme_.pane_item_height;
    const float header_h = theme_.pane_header_height;
    const float content_top = PaneContentTop(r, header_h);
    const float scroll_y = pane.scroll.scroll_y;
    chrome_cmds_.push_back(PushClipCmd{ D2D1::RectF(r.x, content_top, r.x + r.width, r.y + r.height) });

    const int first = std::max(0, static_cast<int>(scroll_y / item_h));
    const int last = std::min(static_cast<int>(item_count) - 1, static_cast<int>((scroll_y + r.height - header_h) / item_h) + 1);
    for (int i = first; i <= last; i++) {
        const float item_y = content_top + i * item_h - scroll_y;
        gen_item(i, D2D1::RectF(r.x, item_y, r.x + r.width, item_y + item_h));
    }

    chrome_cmds_.push_back(PopClipCmd{});
    GenPaneScrollbar(chrome_cmds_, r, header_h, scroll_y, SidePaneContentHeight(item_count, item_h));
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

void Renderer::GenFileExplorer(const std::pmr::vector<FileEntry>& entries, const SidePaneInstance& pane)
{
    constexpr float icon_col_width = 24.0f;
    const float item_h = theme_.pane_item_height;
    GenSidePane(pane, true, entries.size(), i18n::S().pane_header_files, [&](int i, const D2D1_RECT_F& row) {
        const auto& entry = entries[i];

        if (entry.is_current()) {
            chrome_cmds_.push_back(FillRectCmd{ .rect = row, .brush_id = BrushId::PaneItemActive });
        }
        else if (i == pane.hovered_index) {
            chrome_cmds_.push_back(FillRectCmd{ .rect = row, .brush_id = BrushId::PaneItemHover });
        }

        const D2D1_RECT_F icon_rect = D2D1::RectF(row.left + 4.0f, row.top, row.left + 4.0f + icon_col_width, row.bottom);
        EmitChromeText(FileEntryIcon(entry), fmt_.pane_icon.Get(), icon_rect, BrushId::Text, 1.0f, D2D1::SizeF(icon_col_width, item_h));

        EmitLeftText(fmt_.pane_item.Get(), entry.GetDisplayName(), row.left + 4.0f + icon_col_width, row.top, item_h);
    });
}

void Renderer::GenToc(const std::pmr::vector<TocEntry>& entries, const std::pmr::vector<Node>& nodes,
                      const SidePaneInstance& pane, int active_index)
{
    const float item_h = theme_.pane_item_height;
    GenSidePane(pane, false, entries.size(), i18n::S().pane_header_toc, [&](int i, const D2D1_RECT_F& row) {
        const auto& entry = entries[i];

        if (i == active_index || i == pane.hovered_index) {
            const auto bid = (i == active_index) ? BrushId::PaneItemActive : BrushId::PaneItemHover;
            chrome_cmds_.push_back(FillRectCmd{ .rect = row, .brush_id = bid });
        }

        const float text_left = row.left + 8.0f + (entry.heading_level - 1) * TOC_INDENT_PER_LEVEL;
        string_convert::Utf8ToWide(TocItemText(nodes[entry.node_index].GetText()), toc_text_scratch_);
        EmitLeftText(fmt_.pane_item.Get(), toc_text_scratch_, text_left, row.top, item_h);

        if (i == active_index) {
            const float line_y = row.bottom - 1.0f;
            chrome_cmds_.push_back(DrawLineCmd{
                .p0 = D2D1::Point2F(text_left, line_y),
                .p1 = D2D1::Point2F(row.right - 4.0f, line_y),
                .stroke_width = 1.5f,
                .brush_id = BrushId::Text,
            });
        }
    });
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
    rt()->FillRoundedRectangle(VScrollThumbRect(md_pane_rect.x + md_pane_rect.width, ComputeThumbY(info, scroll_y), info.thumb_height), Brush(BrushId::ScrollbarThumb));
}
