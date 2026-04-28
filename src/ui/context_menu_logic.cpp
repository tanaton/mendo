#include "context_menu_impl.h"
#include "theme.h"
#include "i18n.h"
#include "resource.h"

using Microsoft::WRL::ComPtr;
using namespace context_menu_constants;

// ============================================================
// 公開 API（PIMPL 経由の forward）
// ============================================================

ContextMenu::ContextMenu() : impl_(std::make_unique<Impl>()) {}
ContextMenu::~ContextMenu() = default;

void ContextMenu::Init(ID2D1Factory* d2d_factory, IDWriteFactory* dwrite_factory)
{
    impl_->d2d_factory = d2d_factory;
    impl_->dwrite_factory = dwrite_factory;
}

int ContextMenu::HitTest(float x, float y) const noexcept
{
    return impl_->HitTest(x, y);
}

int ContextMenu::NavHitTest(float x, float y) const noexcept
{
    return impl_->NavHitTest(x, y);
}

const std::vector<ContextMenu::Item>& ContextMenu::GetItems() const noexcept
{
    return impl_->items;
}

const ContextMenu::NavRowLayout& ContextMenu::GetNavLayout() const noexcept
{
    return impl_->nav_layout;
}

float ContextMenu::GetMenuWidth() const noexcept
{
    return impl_->menu_width;
}

float ContextMenu::GetMenuHeight() const noexcept
{
    return impl_->menu_height;
}

void ContextMenu::TestBuildItems(const ContextMenuParams& params) { impl_->BuildItems(params); }
void ContextMenu::TestCreateTextFormats(const Theme& theme) { impl_->CreateTextFormats(theme); }
void ContextMenu::TestComputeLayout() { impl_->ComputeLayout(); }

// ============================================================
// メニュー項目構築
// ============================================================

void ContextMenu::Impl::BuildItems(const ContextMenuParams& params)
{
    items.clear();

    Item nav_row;
    nav_row.type = ItemType::NavRow;
    nav_row.id = 0;
    items.emplace_back(std::move(nav_row));
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

// ============================================================
// テキストフォーマット（DWrite only、Win32 / D2D render target 非依存）
// ============================================================

void ContextMenu::Impl::CreateTextFormats(const Theme& t)
{
    if (fmt_text && fmt_icon &&
        cached_fmt_font_family == std::wstring_view{ t.font_family } &&
        cached_fmt_font_size == t.pane_font_size) {
        return;
    }

    fmt_text.Reset();
    fmt_icon.Reset();

    dwrite_factory->CreateTextFormat(
        t.font_family.c_str(), nullptr,
        DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL,
        DWRITE_FONT_STRETCH_NORMAL, t.pane_font_size,
        L"ja-jp", &fmt_text);
    if (fmt_text) {
        fmt_text->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        fmt_text->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    }

    dwrite_factory->CreateTextFormat(
        L"Segoe Fluent Icons", nullptr,
        DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL,
        DWRITE_FONT_STRETCH_NORMAL, ICON_FONT_SIZE,
        L"ja-jp", &fmt_icon);
    if (fmt_icon) {
        fmt_icon->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
        fmt_icon->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        fmt_icon->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    }

    cached_fmt_font_family.assign(t.font_family.begin(), t.font_family.end());
    cached_fmt_font_size = t.pane_font_size;
}

// ============================================================
// レイアウト計算
// ============================================================

void ContextMenu::Impl::ComputeLayout()
{
    float max_text_w = 0.0f;
    for (const auto& item : items) {
        if (item.type != ItemType::Text || item.text.empty()) {
            continue;
        }
        ComPtr<IDWriteTextLayout> layout;
        dwrite_factory->CreateTextLayout(
            item.text.data(), static_cast<UINT32>(item.text.size()),
            fmt_text.Get(), 1000.0f, 100.0f, &layout);
        if (layout) {
            DWRITE_TEXT_METRICS metrics{};
            layout->GetMetrics(&metrics);
            if (metrics.width > max_text_w) {
                max_text_w = metrics.width;
            }
        }
    }

    const float nav_row_w = 2 * NAV_BTN_SIZE + NAV_BTN_GAP + 2 * PAD_X;
    const float text_w = CHECK_WIDTH + max_text_w + PAD_X * 2;
    menu_width = (nav_row_w > text_w) ? nav_row_w : text_w;
    if (menu_width < MIN_MENU_WIDTH) {
        menu_width = MIN_MENU_WIDTH;
    }

    float y = PAD_Y;
    for (auto& item : items) {
        switch (item.type) {
        case ItemType::NavRow: {
            const float row_h = NAV_BTN_SIZE + 2 * NAV_ROW_PAD_Y;
            item.rect = { 0, y, menu_width, y + row_h };

            const float cx = menu_width / 2.0f;
            const float total_w = 2 * NAV_BTN_SIZE + NAV_BTN_GAP;
            const float bx = cx - total_w / 2.0f;
            const float by = y + NAV_ROW_PAD_Y;
            nav_layout.back_rect = { bx, by, bx + NAV_BTN_SIZE, by + NAV_BTN_SIZE };
            nav_layout.fwd_rect = { bx + NAV_BTN_SIZE + NAV_BTN_GAP, by, bx + total_w, by + NAV_BTN_SIZE };
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
        }
    }

    menu_height = y + PAD_Y;
}

// ============================================================
// ヒットテスト
// ============================================================

int ContextMenu::Impl::HitTest(float x, float y) const noexcept
{
    for (const auto& item : items) {
        if (item.type != ItemType::Text || item.id == 0) {
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
