#include "context_menu_impl.h"
#include "i18n.h"
#include "resource.h"
#include <algorithm>
#include <format>
#include <iterator>

using namespace context_menu_constants;

void ContextMenu::Impl::BuildItems(const ContextMenuParams& params)
{
    items.clear();

    items.emplace_back(ItemType::NavRow);
    nav_layout.back_enabled = params.can_go_back;
    nav_layout.fwd_enabled = params.can_go_forward;

    items.emplace_back(ItemType::Separator);

    const auto& ls = i18n::S();
    if (params.show_file_items) {
        items.emplace_back(ItemType::Text, IDM_EDIT_FILE, ls.menu_edit_file, params.has_file, false);
        items.emplace_back(ItemType::Text, IDM_COPY, ls.menu_copy, params.has_selection, false);
        items.emplace_back(ItemType::Text, IDM_COPY_FORMATTED, ls.menu_copy_formatted, params.has_selection, false);
        items.emplace_back(ItemType::Separator);
    }
    zoom = params.zoom;
    on_zoom = params.on_zoom;
    if (zoom) {
        items.emplace_back(ItemType::ZoomRow, 0, ls.menu_zoom);
        items.emplace_back(ItemType::Separator);
    }
    items.emplace_back(ItemType::Text, IDM_TOGGLE_DARK_MODE, ls.menu_dark_mode, true, params.dark_mode_checked);
    items.emplace_back(ItemType::Separator);
    items.emplace_back(ItemType::Text, IDM_TOGGLE_FILE_PANE, ls.menu_file_pane, true, params.file_pane_checked);
    items.emplace_back(ItemType::Text, IDM_TOGGLE_TOC_PANE, ls.menu_toc_pane, true, params.toc_pane_checked);
}

void ContextMenu::Impl::ComputeLayout()
{
    float max_text_w = 0.0f;
    float zoom_row_w = 0.0f;
    for (const auto& item : items) {
        if (item.type == ItemType::ZoomRow) {
            zoom_row_w = PAD_X + Measure(item.text, fmt_text.Get()) + ZOOM_LABEL_GAP + ZOOM_CONTROLS_WIDTH + ZOOM_PAD_RIGHT;
        }
        if (item.type != ItemType::Text || item.text.empty()) {
            continue;
        }
        max_text_w = std::max(max_text_w, Measure(item.text, fmt_text.Get()));
    }

    const float nav_row_w = CTX_NAV_BUTTONS_WIDTH + 2 * PAD_X;
    const float text_w = CHECK_WIDTH + max_text_w + PAD_X * 2;
    width = std::max({ nav_row_w, text_w, zoom_row_w, MIN_MENU_WIDTH });

    float y = PAD_Y;
    for (auto& item : items) {
        switch (item.type) {
        case ItemType::NavRow: {
            const float row_h = CTX_NAV_BTN_SIZE + 2 * CTX_NAV_ROW_PAD_Y;
            item.rect = { 0, y, width, y + row_h };

            const float cx = width / 2.0f;
            const float bx = cx - CTX_NAV_BUTTONS_WIDTH / 2.0f;
            const float by = y + CTX_NAV_ROW_PAD_Y;
            nav_layout.back_rect = { bx, by, bx + CTX_NAV_BTN_SIZE, by + CTX_NAV_BTN_SIZE };
            nav_layout.fwd_rect = { bx + CTX_NAV_BTN_SIZE + CTX_NAV_BTN_GAP, by, bx + CTX_NAV_BUTTONS_WIDTH, by + CTX_NAV_BTN_SIZE };
            y += row_h;
            break;
        }
        case ItemType::ZoomRow: {
            item.rect = { 0, y, width, y + ITEM_HEIGHT };
            // 右端から 元に戻す・拡大・倍率・縮小 の順に詰める。
            const auto box = [y](float right, float w) { return DipRect{ right - w, y, right, y + ZOOM_BTN_SIZE }; };
            zoom_layout.reset_rect = box(width - ZOOM_PAD_RIGHT, ZOOM_BTN_SIZE);
            zoom_layout.in_rect = box(zoom_layout.reset_rect.left - ZOOM_RESET_GAP, ZOOM_BTN_SIZE);
            zoom_layout.percent_rect = box(zoom_layout.in_rect.left, ZOOM_PERCENT_WIDTH);
            zoom_layout.out_rect = box(zoom_layout.percent_rect.left, ZOOM_BTN_SIZE);
            y += ITEM_HEIGHT;
            break;
        }
        case ItemType::Separator:
            item.rect = { 0, y, width, y + SEPARATOR_HEIGHT };
            y += SEPARATOR_HEIGHT;
            break;
        case ItemType::Text:
            item.rect = { 0, y, width, y + ITEM_HEIGHT };
            y += ITEM_HEIGHT;
            break;
        default:
            std::unreachable();
        }
    }

    height = y + PAD_Y;
}

int ContextMenu::Impl::HitTest(float x, float y) const noexcept
{
    for (const auto& item : items) {
        if (item.type != ItemType::Text || item.id == 0 || !item.enabled) {
            continue;
        }
        if (PointInRect(x, y, item.rect)) {
            return item.id;
        }
    }
    return 0;
}

int ContextMenu::Impl::NavHitTest(float x, float y) const noexcept
{
    if (nav_layout.back_enabled && PointInRect(x, y, nav_layout.back_rect)) {
        return IDM_NAV_BACK;
    }
    if (nav_layout.fwd_enabled && PointInRect(x, y, nav_layout.fwd_rect)) {
        return IDM_NAV_FORWARD;
    }
    return 0;
}

std::optional<ZoomDirection> ContextMenu::Impl::ZoomHitTest(float x, float y) const noexcept
{
    if (!zoom) {
        return std::nullopt;
    }
    const std::pair<const DipRect&, ZoomDirection> buttons[] = {
        { zoom_layout.out_rect, ZoomDirection::Out },
        { zoom_layout.in_rect, ZoomDirection::In },
        { zoom_layout.reset_rect, ZoomDirection::Reset },
    };
    for (const auto& [rect, direction] : buttons) {
        if (zoom->CanStep(direction) && PointInRect(x, y, rect)) {
            return direction;
        }
    }
    return std::nullopt;
}

bool ContextMenu::Impl::UpdateHover(float x, float y)
{
    const int nav = NavHitTest(x, y);
    const auto zoom_hit = ZoomHitTest(x, y);
    const int id = nav == 0 && !zoom_hit ? HitTest(x, y) : 0;
    if (nav == hovered_nav && zoom_hit == hovered_zoom && id == hovered_id) {
        return false;
    }
    hovered_nav = nav;
    hovered_zoom = zoom_hit;
    hovered_id = id;
    return true;
}

void ContextMenu::Impl::OnPress(UINT msg, float x, float y)
{
    if (msg == WM_MBUTTONDOWN) {
        return;
    }
    // ズームは閉じずに倍率だけ変え、続けて押せるようにする。端に着いたボタンは押せなくなる。
    if (const auto zoom_hit = ZoomHitTest(x, y)) {
        if (on_zoom) {
            on_zoom(*zoom_hit);
        }
        return;
    }
    const int nav_hit = NavHitTest(x, y);
    const int hit = nav_hit != 0 ? nav_hit : HitTest(x, y);
    if (hit != 0) {
        selected_id = hit;
        done = true;
    }
}

int ContextMenu::Show(HWND owner_hwnd, const ContextMenuParams& params)
{
    Impl s;
    if (!s.Setup(params.env)) {
        return 0;
    }
    s.BuildItems(params);
    s.CreateBaseTextFormats(ICON_FONT_SIZE);
    s.ComputeLayout();
    s.RunModal(owner_hwnd, i18n::S().menu_title, { params.screen_x, params.screen_y });
    return s.selected_id;
}

void ContextMenu::Impl::PaintContent()
{
    for (const auto& item : items) {
        switch (item.type) {
        case ItemType::NavRow:
            DrawNavRow();
            break;
        case ItemType::ZoomRow:
            DrawZoomRow(item);
            break;
        case ItemType::Separator:
            DrawSeparator(item);
            break;
        case ItemType::Text:
            DrawTextItem(item);
            break;
        default:
            std::unreachable();
        }
    }
}

void ContextMenu::Impl::DrawIconButton(const DipRect& rect, const wchar_t* glyph, bool enabled, bool hovered)
{
    if (hovered) {
        FillHover(rect);
    }
    DrawLabel(glyph, fmt_icon.Get(), rect, enabled ? brushes.text.Get() : brushes.gray.Get());
}

void ContextMenu::Impl::DrawNavRow()
{
    DrawIconButton(nav_layout.back_rect, GLYPH_BACK, nav_layout.back_enabled, hovered_nav == IDM_NAV_BACK);
    DrawIconButton(nav_layout.fwd_rect, GLYPH_FORWARD, nav_layout.fwd_enabled, hovered_nav == IDM_NAV_FORWARD);
}

void ContextMenu::Impl::DrawZoomRow(const Item& item)
{
    if (!zoom) {
        return;
    }
    DrawLabel(item.text, fmt_text.Get(), { item.rect.left + PAD_X, item.rect.top, zoom_layout.out_rect.left, item.rect.bottom }, brushes.text.Get());

    const auto draw_btn = [&](const DipRect& rect, const wchar_t* glyph, ZoomDirection direction) {
        const bool enabled = zoom->CanStep(direction);
        DrawIconButton(rect, glyph, enabled, enabled && hovered_zoom == direction);
    };
    draw_btn(zoom_layout.out_rect, GLYPH_ZOOM_OUT, ZoomDirection::Out);
    draw_btn(zoom_layout.in_rect, GLYPH_ZOOM_IN, ZoomDirection::In);
    draw_btn(zoom_layout.reset_rect, GLYPH_ZOOM_RESET, ZoomDirection::Reset);

    wchar_t percent[16];
    const auto r = std::format_to_n(percent, std::size(percent), L"{}%", zoom->Percent());
    const size_t len = std::min(static_cast<size_t>(r.size), std::size(percent));
    DrawLabel({ percent, len }, fmt_text_center.Get(), zoom_layout.percent_rect, brushes.text.Get());
}

void ContextMenu::Impl::DrawSeparator(const Item& item)
{
    const float cy = (item.rect.top + item.rect.bottom) / 2.0f;
    rt->DrawLine(
        { item.rect.left + SEPARATOR_MARGIN_X, cy },
        { item.rect.right - SEPARATOR_MARGIN_X, cy },
        brushes.border.Get(),
        1.0f);
}

void ContextMenu::Impl::DrawTextItem(const Item& item)
{
    if (item.id != 0 && item.id == hovered_id) {
        FillHover({ item.rect.left + HOVER_MARGIN_X, item.rect.top + 1.0f, item.rect.right - HOVER_MARGIN_X, item.rect.bottom - 1.0f });
    }
    if (item.checked) {
        DrawCheck({ item.rect.left + CHECK_LEFT, item.rect.top, item.rect.left + CHECK_WIDTH + 4.0f, item.rect.bottom });
    }
    DrawLabel(item.text, fmt_text.Get(), { item.rect.left + PAD_X, item.rect.top, item.rect.right - TEXT_PAD_RIGHT, item.rect.bottom },
              item.enabled ? brushes.text.Get() : brushes.gray.Get());
}
