#include "context_menu_impl.h"
#include "d2d_util.h"
#include "theme.h"
#include "i18n.h"
#include "resource.h"
#include <algorithm>

using Microsoft::WRL::ComPtr;
using namespace context_menu_constants;

ContextMenu::ContextMenu() : impl_(std::make_unique<Impl>())
{}
ContextMenu::~ContextMenu() = default;

void ContextMenu::Init(ID2D1Factory* d2d_factory, IDWriteFactory* dwrite_factory)
{
    impl_->d2d_factory = d2d_factory;
    impl_->dwrite_factory = dwrite_factory;
}

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
    items.emplace_back(ItemType::Text, IDM_TOGGLE_DARK_MODE, ls.menu_dark_mode, true, params.dark_mode_checked);
    items.emplace_back(ItemType::Separator);
    items.emplace_back(ItemType::Text, IDM_TOGGLE_FILE_PANE, ls.menu_file_pane, true, params.file_pane_checked);
    items.emplace_back(ItemType::Text, IDM_TOGGLE_TOC_PANE, ls.menu_toc_pane, true, params.toc_pane_checked);
}

void ContextMenu::Impl::CreateTextFormats(const Theme& t)
{
    if (fmt_text && fmt_icon &&
        cached_fmt_font_family == std::wstring_view{ t.font_family } &&
        cached_fmt_icon_font == std::wstring_view{ t.icon_font } &&
        cached_fmt_font_size == t.pane_font_size) {
        return;
    }

    // 項目の文字はサイドペインの項目 (Renderer の pane_item) と同じ書式にする。
    constexpr auto CENTER = DWRITE_PARAGRAPH_ALIGNMENT_CENTER;
    fmt_text = CreateTextFormat(dwrite_factory, { .family = t.font_family.c_str(), .size = t.pane_font_size, .para_align = CENTER, .no_wrap = true });
    fmt_icon = CreateTextFormat(dwrite_factory, { .family = t.icon_font.c_str(), .size = ICON_FONT_SIZE, .text_align = DWRITE_TEXT_ALIGNMENT_CENTER, .para_align = CENTER, .no_wrap = true });

    cached_fmt_font_family.assign(t.font_family.begin(), t.font_family.end());
    cached_fmt_icon_font.assign(t.icon_font.begin(), t.icon_font.end());
    cached_fmt_font_size = t.pane_font_size;
    text_widths.clear();
}

float ContextMenu::Impl::TextWidth(std::wstring_view text)
{
    if (const auto it = text_widths.find(text.data()); it != text_widths.end()) {
        return it->second;
    }
    float width = 0.0f;
    ComPtr<IDWriteTextLayout> layout;
    dwrite_factory->CreateTextLayout(
        text.data(), static_cast<UINT32>(text.size()),
        fmt_text.Get(), 1000.0f, 100.0f, &layout);
    if (layout) {
        DWRITE_TEXT_METRICS metrics{};
        layout->GetMetrics(&metrics);
        width = metrics.width;
        text_widths.emplace(text.data(), width);
    }
    return width;
}

void ContextMenu::Impl::ComputeLayout()
{
    float max_text_w = 0.0f;
    for (const auto& item : items) {
        if (item.type != ItemType::Text || item.text.empty()) {
            continue;
        }
        max_text_w = std::max(max_text_w, TextWidth(item.text));
    }

    const float nav_row_w = CTX_NAV_BUTTONS_WIDTH + 2 * PAD_X;
    const float text_w = CHECK_WIDTH + max_text_w + PAD_X * 2;
    menu_width = std::max({ nav_row_w, text_w, MIN_MENU_WIDTH });

    float y = PAD_Y;
    for (auto& item : items) {
        switch (item.type) {
        case ItemType::NavRow: {
            const float row_h = CTX_NAV_BTN_SIZE + 2 * CTX_NAV_ROW_PAD_Y;
            item.rect = { 0, y, menu_width, y + row_h };

            const float cx = menu_width / 2.0f;
            const float bx = cx - CTX_NAV_BUTTONS_WIDTH / 2.0f;
            const float by = y + CTX_NAV_ROW_PAD_Y;
            nav_layout.back_rect = { bx, by, bx + CTX_NAV_BTN_SIZE, by + CTX_NAV_BTN_SIZE };
            nav_layout.fwd_rect = { bx + CTX_NAV_BTN_SIZE + CTX_NAV_BTN_GAP, by, bx + CTX_NAV_BUTTONS_WIDTH, by + CTX_NAV_BTN_SIZE };
            y += row_h;
            break;
        }
        case ItemType::Separator:
            item.rect = { 0, y, menu_width, y + SEPARATOR_HEIGHT };
            y += SEPARATOR_HEIGHT;
            break;
        case ItemType::Text:
            item.rect = { 0, y, menu_width, y + ITEM_HEIGHT };
            y += ITEM_HEIGHT;
            break;
        default:
            std::unreachable();
        }
    }

    menu_height = y + PAD_Y;
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
