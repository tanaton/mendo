#include "app_controller.h"
#include "i18n.h"
#include "string_convert.h"
#include "titlebar.h"
#include "tooltip.h"
#include "ui_constants.h"
#include <optional>
#include <string_view>
#include <utility>
#include <windows.h>

namespace app_controller {

AppAction HandleKeyDown(const KeyDownEvent& event)
{
    if (event.alt && !event.ctrl) {
        switch (event.key) {
        case VK_LEFT:
            return NavigateBackAction{};
        case VK_RIGHT:
            return NavigateForwardAction{};
        }
        return NoOpAction{};
    }

    if (event.ctrl) {
        switch (event.key) {
        case 'C':
            if (event.shift) {
                return CopyFormattedClipboardAction{};
            }
            return CopyClipboardAction{};
        case 'A':
            return SelectAllAction{};
        case 'O':
            return OpenFileAction{};
        case 'F':
            return OpenSearchBarAction{};
        case 'G':
            if (event.shift) {
                return SearchPrevAction{};
            }
            return SearchNextAction{};
        case '1':
            return TogglePaneAction{ PaneTarget::File };
        case '2':
            return TogglePaneAction{ PaneTarget::Toc };
        case VK_OEM_PLUS:
        case VK_ADD:
            return ZoomAction{ ZoomDirection::In };
        case VK_OEM_MINUS:
        case VK_SUBTRACT:
            return ZoomAction{ ZoomDirection::Out };
        case '0':
        case VK_NUMPAD0:
            return ZoomAction{ ZoomDirection::Reset };
        }
        return NoOpAction{};
    }

    switch (event.key) {
    case VK_UP:
        return KeyScrollAction{ ScrollType::LineUp };
    case VK_DOWN:
        return KeyScrollAction{ ScrollType::LineDown };
    case VK_PRIOR:
        return KeyScrollAction{ ScrollType::PageUp };
    case VK_NEXT:
        return KeyScrollAction{ ScrollType::PageDown };
    case VK_HOME:
        return KeyScrollAction{ ScrollType::Home };
    case VK_END:
        return KeyScrollAction{ ScrollType::End };
    case VK_F1:
        return ShowHelpAction{};
    case VK_F3:
        if (event.shift) {
            return SearchPrevAction{};
        }
        return SearchNextAction{};
    case VK_F5:
        return ReloadFileAction{};
    case VK_ESCAPE:
        return ClearSelectionAction{};
    }

    return NoOpAction{};
}

namespace {

// 入力欄が編集に使うキー: キャレット移動・削除と、Ctrl を付けたコピー・貼り付け・切り取り・元に戻す。
// 単行 EDIT の ↑↓ は ←→ と同じ動きしかしないので、本体のスクロールに回す。
constexpr bool IsSearchEditOwnKey(const KeyDownEvent& event) noexcept
{
    if (event.alt) {
        return false;
    }
    switch (event.key) {
    case VK_LEFT:
    case VK_RIGHT:
    case VK_HOME:
    case VK_END:
    case VK_BACK:
    case VK_DELETE:
    case VK_INSERT:
        return true;
    case 'C':
    case 'V':
    case 'X':
    case 'Z':
    case 'Y':
        return event.ctrl;
    }
    return false;
}

} // namespace

SearchEditKeyResult HandleSearchEditKeyDown(const KeyDownEvent& event)
{
    if (!event.ctrl && !event.alt && event.key == VK_RETURN) {
        return { SearchEditKey::App, event.shift ? AppAction{ SearchPrevAction{} } : AppAction{ SearchNextAction{} } };
    }
    if (event.ctrl && !event.alt && event.key == 'A') {
        return { SearchEditKey::SelectAll };
    }
    if (IsSearchEditOwnKey(event)) {
        return { SearchEditKey::Edit };
    }
    // Esc と Ctrl+F は、本体の割り当てでも検索欄が開いていれば閉じる。
    AppAction action = HandleKeyDown(event);
    if (std::holds_alternative<NoOpAction>(action)) {
        return { SearchEditKey::Edit };
    }
    return { SearchEditKey::App, std::move(action) };
}

namespace {

// button はペイン左上を原点とする矩形。
constexpr bool HitPaneHeaderButton(float dip_x, float dip_y, const PaneRect& rect, float header_height, const D2D1_RECT_F& button)
{
    const float local_x = dip_x - rect.x;
    const float local_y = dip_y - rect.y;
    if (local_y >= header_height) {
        return false;
    }
    return PointInRect(local_x, local_y, button);
}

constexpr PaneHeaderButton HitSidePaneHeaderButton(float dip_x, float dip_y, const PaneRect& rect, float header_h, SidePaneHeaderButtons buttons)
{
    if (HitPaneHeaderButton(dip_x, dip_y, rect, header_h, PaneCloseButtonRect(rect.width, header_h))) {
        return PaneHeaderButton::Close;
    }
    if (!buttons.has_file_buttons) {
        return PaneHeaderButton::None;
    }
    if (HitPaneHeaderButton(dip_x, dip_y, rect, header_h, PaneRefreshButtonRect(rect.width, header_h))) {
        return PaneHeaderButton::Refresh;
    }
    if (buttons.reveal_enabled && HitPaneHeaderButton(dip_x, dip_y, rect, header_h, PaneRevealButtonRect(rect.width, header_h))) {
        return PaneHeaderButton::Reveal;
    }
    return PaneHeaderButton::None;
}

bool IsOverPaneScrollbar(float dip_x, const PaneRect& rect, const PaneScrollInfo& scroll_info, bool may_grow = false) noexcept
{
    return ShowsVScrollbar(scroll_info, may_grow) && InVScrollbarHitX(dip_x, rect.x + rect.width);
}

std::wstring_view OverlayButtonTooltipText(OverlayButton kind) noexcept
{
    switch (kind) {
    case OverlayButton::DiagramCopy:
        return i18n::S().tooltip_copy_diagram;
    case OverlayButton::DiagramSave:
        return i18n::S().tooltip_save_image;
    case OverlayButton::CodeCopy:
    case OverlayButton::None:
        return i18n::S().tooltip_copy;
    }
    std::unreachable();
}

std::wstring_view TitleBarTooltipText(TitleBarHitZone zone, bool is_maximized) noexcept
{
    const auto& ls = i18n::S();
    switch (zone) {
    case TitleBarHitZone::None:
    case TitleBarHitZone::Caption:
    case TitleBarHitZone::Icon:
        return {};
    case TitleBarHitZone::OpenFile:
        return ls.tooltip_open_file;
    case TitleBarHitZone::Help:
        return ls.tooltip_help;
    case TitleBarHitZone::Settings:
        return ls.settings_title;
    case TitleBarHitZone::ThemeToggle:
        return ls.tooltip_theme_toggle;
    case TitleBarHitZone::Search:
        return ls.tooltip_search;
    case TitleBarHitZone::FileToggle:
        return ls.tooltip_file_pane;
    case TitleBarHitZone::TocToggle:
        return ls.tooltip_toc_pane;
    case TitleBarHitZone::Minimize:
        return ls.tooltip_minimize;
    case TitleBarHitZone::Maximize:
        return is_maximized ? ls.tooltip_restore : ls.tooltip_maximize;
    case TitleBarHitZone::Close:
        return ls.tooltip_close;
    }
    std::unreachable();
}

std::wstring_view SearchBarTooltipText(SearchBarHitZone zone) noexcept
{
    const auto& ls = i18n::S();
    switch (zone) {
    case SearchBarHitZone::Up:
        return ls.tooltip_search_prev;
    case SearchBarHitZone::Down:
        return ls.tooltip_search_next;
    case SearchBarHitZone::CaseSensitive:
        return ls.tooltip_search_case;
    case SearchBarHitZone::Highlight:
        return ls.tooltip_search_highlight;
    case SearchBarHitZone::Close:
        return ls.tooltip_search_close;
    case SearchBarHitZone::None:
    case SearchBarHitZone::Input:
        return {};
    }
    std::unreachable();
}

std::wstring_view NavButtonTooltipText(NavDirection hit) noexcept
{
    switch (hit) {
    case NavDirection::None:
        return {};
    case NavDirection::Back:
        return i18n::S().tooltip_nav_back;
    case NavDirection::Forward:
        return i18n::S().tooltip_nav_forward;
    }
    std::unreachable();
}

std::wstring_view PaneHeaderButtonTooltipText(PaneHeaderButton hit) noexcept
{
    switch (hit) {
    case PaneHeaderButton::None:
        return {};
    case PaneHeaderButton::Close:
        return i18n::S().tooltip_pane_close;
    case PaneHeaderButton::Refresh:
        return i18n::S().tooltip_pane_refresh;
    case PaneHeaderButton::Reveal:
        return i18n::S().tooltip_pane_reveal;
    }
    std::unreachable();
}

// 上位 32bit に親 (node index・一覧の世代)、下位に子 (リンク index・項目 index) を詰める。
constexpr uint64_t PackTooltipKey(uint32_t outer, uint32_t inner) noexcept
{
    return (uint64_t{ outer } << 32) | inner;
}

TooltipHover BuildMdContentTooltip(const AppState& state, int hit_node, std::optional<LinkHit> link)
{
    if (hit_node < 0) {
        return {};
    }
    const auto& current = state.interaction.tooltip;
    const auto& node = state.document.doc.GetNodes()[hit_node];
    const auto node_index = static_cast<uint32_t>(hit_node);
    if (link) {
        const uint64_t key = PackTooltipKey(node_index, link->index);
        return MakeTooltip(current, TooltipTarget::Zone::MdLink, key, [&](std::wstring& text) {
            string_convert::Utf8ToWide(link->url, text);
        });
    }
    auto* const img = node.image_data();
    if (!img || node.type != NodeType::Image) {
        return {};
    }
    const std::string_view alt = node.GetText();
    return MakeTooltip(current, TooltipTarget::Zone::MdImage, node_index, [&](std::wstring& text) {
        if (!alt.empty()) {
            string_convert::Utf8ToWide(alt, text);
            text += L"\n";
        }
        std::wstring src_wide;
        string_convert::Utf8ToWide(img->src, src_wide);
        text += src_wide;
    });
}

TooltipHover BuildSidePaneTooltip(const AppState& state, PaneTarget target, PaneHeaderButton hit, int idx)
{
    const bool is_file = target == PaneTarget::File;
    const auto& current = state.interaction.tooltip;
    if (hit != PaneHeaderButton::None) {
        const auto zone = is_file ? TooltipTarget::Zone::FilePaneButton : TooltipTarget::Zone::TocPaneButton;
        return MakeTooltip(current, zone, std::to_underlying(hit), PaneHeaderButtonTooltipText(hit));
    }
    if (idx < 0) {
        return {};
    }
    const auto item = static_cast<uint32_t>(idx);
    if (is_file) {
        const auto& entries = state.file_explorer.GetEntries();
        if (item >= entries.size()) {
            return {};
        }
        // 一覧は列挙し直すと並びが変わるため世代を含める。
        const uint64_t key = PackTooltipKey(state.file_explorer.GetGeneration(), item);
        return MakeTooltip(current, TooltipTarget::Zone::FilePaneItem, key, [&](std::wstring& text) {
            text = entries[item].full_path;
        });
    }
    const auto& toc_entries = state.document.doc.GetToc();
    if (item >= toc_entries.size()) {
        return {};
    }
    return MakeTooltip(current, TooltipTarget::Zone::TocPaneItem, item, [&](std::wstring& text) {
        string_convert::Utf8ToWide(state.document.doc.GetNodes()[toc_entries[item].node_index].GetText(), text);
    });
}

void HoverSidePane(const AppState& state, PaneTarget target, const PaneLayout& layout, float dip_x, float dip_y, PointerHoverResult& result)
{
    const auto hit = ResolveSidePaneHit(state, target, layout, dip_x, dip_y);
    const auto k = std::to_underlying(target);
    result.hover.side_button[k] = hit.button;
    result.hover.side_index[k] = hit.item;
    result.hover.tooltip = BuildSidePaneTooltip(state, target, hit.button, hit.item);
    result.cursor = hit.button != PaneHeaderButton::None ? PointerCursor::Hand : PointerCursor::Arrow;
}

void HoverMdPane(const AppState& state, const PaneLayout& layout, float dip_x, float dip_y, int px, int py, PointerHoverResult& result)
{
    auto& hover = result.hover;
    const auto& current_tooltip = state.interaction.tooltip;
    const auto hit_ctx = MakeMdPaneHitContext(state, layout, px, py);
    const auto target = ResolveMdPaneHit(state, hit_ctx, layout, dip_x, dip_y);
    switch (target.kind) {
    case MdPaneHit::Kind::SearchBar:
        hover.search_bar = target.search_bar;
        hover.tooltip = MakeTooltip(current_tooltip, TooltipTarget::Zone::SearchBarButton, std::to_underlying(hover.search_bar), SearchBarTooltipText(hover.search_bar));
        result.cursor = hover.search_bar == SearchBarHitZone::Input ? PointerCursor::IBeam : PointerCursor::Arrow;
        return;
    case MdPaneHit::Kind::NavButton:
        hover.nav = target.nav;
        hover.tooltip = MakeTooltip(current_tooltip, TooltipTarget::Zone::NavButton, std::to_underlying(hover.nav), NavButtonTooltipText(hover.nav));
        result.cursor = PointerCursor::Hand;
        return;
    case MdPaneHit::Kind::Scrollbar:
        result.cursor = PointerCursor::Arrow;
        return;
    case MdPaneHit::Kind::OverlayButton:
    case MdPaneHit::Kind::Content:
        break;
    }

    const int node_index = hit_test::HitNode(hit_ctx);
    // ボタンは画像の右上の内側にあるので、ボタンの上でも画像の上と判定される。
    if (hit_test::DiagramImageHitTest(hit_ctx, node_index)) {
        hover.diagram = node_index;
    }
    // 横スクロールできるブロック (Table / CodeBlock) だけバーを出す。
    if (state.view.HDragNode() < 0 && ResolveBlockHScrollGeometry(state, node_index).can_scroll()) {
        hover.h_block = node_index;
    }

    hover.overlay_button = target.overlay_button;
    if (hover.overlay_button.Any()) {
        const auto kind = hover.overlay_button.kind;
        hover.tooltip = MakeTooltip(current_tooltip, TooltipTarget::Zone::OverlayButton, std::to_underlying(kind), OverlayButtonTooltipText(kind));
        result.cursor = PointerCursor::Hand;
        return;
    }

    // 文字位置はリンク判定にしか使わないので、リンクを含むノードの上でだけ求める。
    std::optional<LinkHit> link;
    if (node_index >= 0 && !state.document.doc.GetNodes()[node_index].view_link_urls().empty()) {
        link = LinkAtHit(state, hit_test::HitTest(hit_ctx));
    }
    hover.tooltip = BuildMdContentTooltip(state, node_index, link);
    result.cursor = link ? PointerCursor::Hand : PointerCursor::IBeam;
}

} // namespace

MdPaneHitContext MakeMdPaneHitContext(const AppState& state, const PaneLayout& layout, int px, int py) noexcept
{
    const Theme& theme = *state.theme;
    return MdPaneHitContext{
        .nodes = state.document.doc.GetNodes(),
        .cache = state.document.layout_cache,
        .theme = theme,
        .scroll_y = state.view.viewport.GetScrollY(),
        .md_rect = layout.md_rect,
        .dpi_scale = state.window.cached_dpi_scale,
        .screen_x = px,
        .screen_y = py,
        .content_width = theme.ContentWidth(layout.md_rect.width),
        .block_scroll_x = &state.view.block_scroll_x,
    };
}

std::optional<LinkHit> LinkAtHit(const AppState& state, const hit_test::HitResult& hit)
{
    const auto& nodes = state.document.doc.GetNodes();
    if (hit.node_index < 0 || hit.node_index >= static_cast<int>(nodes.size())) {
        return std::nullopt;
    }
    return FindLinkHitAtPosition(nodes[hit.node_index], hit.char_pos);
}

bool IsOverMdScrollbar(const AppState& state, const PaneLayout& layout, float dip_x, float dip_y) noexcept
{
    const PaneRect& md = layout.md_rect;
    if (dip_y < md.y || dip_y > md.y + md.height) {
        return false;
    }
    return IsOverPaneScrollbar(dip_x, md, MdScrollInfo(state, layout), state.document.layout_cache.HasDirtyNodes());
}

MdPaneHit ResolveMdPaneHit(const AppState& state, const MdPaneHitContext& hit_ctx, const PaneLayout& layout, float dip_x, float dip_y) noexcept
{
    using Kind = MdPaneHit::Kind;
    if (state.search.search_state.IsVisible()) {
        if (const auto sbl = SearchBarLayoutFor(state, layout.md_rect); dip_y >= sbl.bar_top) {
            return { .kind = Kind::SearchBar, .search_bar = HitTestSearchBar(sbl, dip_x, dip_y) };
        }
    }
    if (const auto nav = hit_test::NavButtonHitTest(dip_x, dip_y, layout.md_rect); nav != NavDirection::None) {
        return { .kind = Kind::NavButton, .nav = nav };
    }
    // 縮小表示で余白が狭いとボタンがスクロールバーのヒット余白に掛かるので、ボタンを優先する。
    if (const auto button = hit_test::OverlayButtonHitTest(hit_ctx); button.Any()) {
        return { .kind = Kind::OverlayButton, .overlay_button = button };
    }
    if (IsOverMdScrollbar(state, layout, dip_x, dip_y)) {
        return { .kind = Kind::Scrollbar };
    }
    return {};
}

SidePaneHit ResolveSidePaneHit(const AppState& state, PaneTarget target, const PaneLayout& layout, float dip_x, float dip_y) noexcept
{
    const float header_h = state.theme->pane_header_height;
    const PaneRect& rect = layout.Get(target);
    SidePaneHit hit{
        .in_header = dip_y - rect.y < header_h,
        .button = HitSidePaneHeaderButton(dip_x, dip_y, rect, header_h, SidePaneHeaderButtonsFor(state, target)),
        .item = SidePaneItemAt(state, target, rect, dip_y),
    };
    if (!hit.in_header) {
        hit.scrollbar = IsOverPaneScrollbar(dip_x, rect, SidePaneScrollInfo(state, target, layout));
    }
    return hit;
}

PointerHoverResult ResolvePointerHover(const AppState& state, const PaneLayout& layout, int px, int py, bool maximized)
{
    const auto dip = PixelToDip(px, py, state.window.cached_dpi_scale);
    PointerHoverResult result;
    if (dip.y < TitleBar::BASE_HEIGHT) {
        auto& hover = result.hover;
        hover.titlebar = state.window.titlebar.HitTest(dip.x, dip.y);
        // 最大化ボタンは状態で文言が変わるため key を分ける。
        const uint64_t key = std::to_underlying(hover.titlebar) * 2ull + (hover.titlebar == TitleBarHitZone::Maximize && maximized);
        hover.tooltip = MakeTooltip(state.interaction.tooltip, TooltipTarget::Zone::TitleBarButton, key, TitleBarTooltipText(hover.titlebar, maximized));
        return result;
    }
    switch (const auto zone = DetectPaneZone(dip.x, layout, state.theme->splitter_width)) {
    case PaneZone::Splitter1:
    case PaneZone::Splitter2:
        result.cursor = PointerCursor::SizeWE;
        break;
    case PaneZone::FilePane:
    case PaneZone::TocPane:
        HoverSidePane(state, *ToPaneTarget(zone), layout, dip.x, dip.y, result);
        break;
    case PaneZone::MdPane:
        HoverMdPane(state, layout, dip.x, dip.y, px, py, result);
        break;
    case PaneZone::None:
        break;
    }
    return result;
}

} // namespace app_controller
