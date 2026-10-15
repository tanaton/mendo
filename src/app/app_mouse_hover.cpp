#include "app.h"
#include "app_mouse_helpers.h"
#include "app_state_queries.h"
#include "block_h_scroll.h"
#include "i18n.h"
#include "pane_layout.h"
#include "string_convert.h"
#include "titlebar.h"
#include "tooltip.h"
#include "ui_constants.h"
#include <string_view>
#include <utility>

namespace {

// text が空 (該当ボタンなし) なら空のターゲットを返す。
TooltipTarget MakeButtonTooltip(const TooltipTarget& current, TooltipTarget::Zone zone, uint64_t key, std::wstring_view text)
{
    return text.empty() ? TooltipTarget{} : MakeTooltip(current, zone, key, text);
}

TooltipTarget BuildTitleBarTooltip(const TooltipTarget& current, TitleBarHitZone zone, bool is_maximized)
{
    const auto& ls = i18n::S();
    const std::wstring_view text = [&]() -> std::wstring_view {
        switch (zone) {
        case TitleBarHitZone::OpenFile:
            return ls.tooltip_open_file;
        case TitleBarHitZone::Help:
            return ls.tooltip_help;
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
        default:
            return {};
        }
    }();
    // 最大化ボタンは状態で文言が変わるため key を分ける。
    const uint64_t key = std::to_underlying(zone) * 2ull + (zone == TitleBarHitZone::Maximize && is_maximized);
    return MakeButtonTooltip(current, TooltipTarget::Zone::TitleBarButton, key, text);
}

TooltipTarget BuildSearchBarTooltip(const TooltipTarget& current, SearchBarHitZone zone)
{
    const auto& ls = i18n::S();
    const std::wstring_view text = [&]() -> std::wstring_view {
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
        default:
            return {};
        }
    }();
    return MakeButtonTooltip(current, TooltipTarget::Zone::SearchBarButton, std::to_underlying(zone), text);
}

TooltipTarget BuildNavButtonTooltip(const TooltipTarget& current, NavButtonHover hit)
{
    const auto& ls = i18n::S();
    const std::wstring_view text = [&]() -> std::wstring_view {
        switch (hit) {
        case NavButtonHover::Back:
            return ls.tooltip_nav_back;
        case NavButtonHover::Forward:
            return ls.tooltip_nav_forward;
        default:
            return {};
        }
    }();
    return MakeButtonTooltip(current, TooltipTarget::Zone::NavButton, std::to_underlying(hit), text);
}

// 上位 32bit に親 (node index・一覧の世代)、下位に子 (リンク index・項目 index) を詰める。
constexpr uint64_t PackTooltipKey(uint32_t outer, uint32_t inner) noexcept
{
    return (uint64_t{ outer } << 32) | inner;
}

} // namespace

bool App::IsOverMdScrollbar(float dip_x, float dip_y, const PaneLayout& layout) const noexcept
{
    const float total_h = ScrollableContentHeight();
    const float viewport_h = layout.md_rect.height;
    if (total_h <= viewport_h || viewport_h <= 0.0f) {
        return false;
    }
    if (dip_y < layout.md_rect.y || dip_y > layout.md_rect.y + viewport_h) {
        return false;
    }
    const float md_right = layout.md_rect.x + layout.md_rect.width;
    return dip_x >= VScrollbarHitLeftX(md_right) && dip_x <= md_right - PANE_SCROLLBAR_MARGIN;
}

bool App::IsOverMdScrollbar(float dip_x, float dip_y)
{
    return IsOverMdScrollbar(dip_x, dip_y, GetPaneLayout());
}

TooltipTarget App::BuildMdContentTooltip(int hit_node, std::optional<LinkHit> link) const
{
    if (hit_node < 0) {
        return {};
    }
    const auto& current = state_.interaction.tooltip.GetCurrent();
    const auto& node = state_.document.doc.GetNodes()[hit_node];
    const auto node_index = static_cast<uint32_t>(hit_node);
    if (link) {
        const uint64_t key = PackTooltipKey(node_index, link->index);
        return MakeTooltip(current, TooltipTarget::Zone::MdLink, key, [&](std::pmr::wstring& text) {
            string_convert::Utf8ToWide(link->url, text);
        });
    }
    auto* const img = node.image_data();
    if (!img || node.type != NodeType::Image) {
        return {};
    }
    const std::string_view alt = node.GetText();
    return MakeTooltip(current, TooltipTarget::Zone::MdImage, node_index, [&](std::pmr::wstring& text) {
        if (!alt.empty()) {
            string_convert::Utf8ToWide(alt, text);
            text += L"\n";
        }
        std::pmr::wstring src_wide;
        string_convert::Utf8ToWide(img->src, src_wide);
        text += src_wide;
    });
}

App::MdPaneHit App::ResolveMdPaneHit(float dip_x, float dip_y, const MdPaneHitContext& hit_ctx, const PaneLayout& pane_layout) const
{
    using Kind = MdPaneHit::Kind;
    if (const auto sbl = SearchBarLayoutAt(dip_y, pane_layout.md_rect)) {
        return { .kind = Kind::SearchBar, .search_bar = HitTestSearchBar(*sbl, dip_x, dip_y) };
    }
    if (const auto nav = hit_test::NavButtonHitTest(dip_x, dip_y, pane_layout.md_rect); nav != NavButtonHover::None) {
        return { .kind = Kind::NavButton, .nav = nav };
    }
    // 1 回の可視ノード走査でコピー/保存/ダイアグラムコピーボタンを同時に判定する。
    // 縮小表示で余白が狭いとボタンがスクロールバーのヒット余白に掛かるので、ボタンを優先する。
    if (const auto buttons = hit_test::CodeBlockButtonsHitTest(hit_ctx); buttons.Any()) {
        return { .kind = Kind::OverlayButton, .buttons = buttons };
    }
    if (IsOverMdScrollbar(dip_x, dip_y, pane_layout)) {
        return { .kind = Kind::Scrollbar };
    }
    return {};
}

HCURSOR App::HoverMdPane(float dip_x, float dip_y, int px, int py, const PaneLayout& pane_layout, PointerHover& hover) const
{
    const auto& current_tooltip = state_.interaction.tooltip.GetCurrent();
    const auto hit_ctx = BuildMdPaneHitContext(px, py, pane_layout);
    const auto target = ResolveMdPaneHit(dip_x, dip_y, hit_ctx, pane_layout);
    switch (target.kind) {
    case MdPaneHit::Kind::SearchBar:
        hover.search_bar = target.search_bar;
        hover.tooltip = BuildSearchBarTooltip(current_tooltip, hover.search_bar);
        return hover.search_bar == SearchBarHitZone::Input ? cursors_.IBeam() : cursors_.Arrow();
    case MdPaneHit::Kind::NavButton:
        hover.nav = target.nav;
        hover.tooltip = BuildNavButtonTooltip(current_tooltip, hover.nav);
        return cursors_.Hand();
    case MdPaneHit::Kind::Scrollbar:
        return cursors_.Arrow();
    case MdPaneHit::Kind::OverlayButton:
    case MdPaneHit::Kind::Content:
        break;
    }

    const int node_index = hit_test::HitNode(hit_ctx);
    // 横スクロール対象 (Table / CodeBlock) で自然幅 > 可視幅 のときだけバーを出す。
    if (state_.view.h_drag_node < 0 && ResolveBlockHScrollGeometry(state_, node_index).can_scroll()) {
        hover.h_block = node_index;
    }

    const auto& btn_hit = target.buttons;
    hover.buttons = HoveredButtons{ btn_hit.copy_node, btn_hit.save_node, btn_hit.diagram_copy_node };
    const auto button_tooltip = [&](TooltipTarget::Zone zone, std::wstring_view text) {
        hover.tooltip = MakeTooltip(current_tooltip, zone, 0, text);
        return cursors_.Hand();
    };
    if (hover.buttons.copy >= 0) {
        return button_tooltip(TooltipTarget::Zone::CopyButton, i18n::S().tooltip_copy);
    }
    if (hover.buttons.diagram_copy >= 0) {
        return button_tooltip(TooltipTarget::Zone::DiagramCopyButton, i18n::S().tooltip_copy_diagram);
    }
    if (hover.buttons.save >= 0) {
        return button_tooltip(TooltipTarget::Zone::SaveButton, i18n::S().tooltip_save_image);
    }

    // 文字位置はリンク判定にしか使わないので、リンクを含むノードの上でだけ求める。
    std::optional<LinkHit> link;
    if (node_index >= 0 && !state_.document.doc.GetNodes()[node_index].view_link_urls().empty()) {
        link = GetLinkAtHit(hit_test::HitTest(hit_ctx));
    }
    hover.tooltip = BuildMdContentTooltip(node_index, link);
    return link ? cursors_.Hand() : cursors_.IBeam();
}

TooltipTarget App::BuildSidePaneTooltip(PaneTarget target, PaneHeaderButton hit, int idx) const
{
    const bool is_file = target == PaneTarget::File;
    const auto& current = state_.interaction.tooltip.GetCurrent();
    if (hit != PaneHeaderButton::None) {
        const auto& ls = i18n::S();
        const std::wstring_view text = hit == PaneHeaderButton::Close     ? ls.tooltip_pane_close
                                       : hit == PaneHeaderButton::Refresh ? ls.tooltip_pane_refresh
                                                                          : ls.tooltip_pane_reveal;
        const auto zone = is_file ? TooltipTarget::Zone::FilePaneButton : TooltipTarget::Zone::TocPaneButton;
        return MakeTooltip(current, zone, std::to_underlying(hit), text);
    }
    if (idx < 0) {
        return {};
    }
    const auto item = static_cast<uint32_t>(idx);
    if (is_file) {
        const auto& entries = state_.file_explorer.GetEntries();
        if (item >= entries.size()) {
            return {};
        }
        // 一覧は列挙し直すと並びが変わるため世代を含める。
        const uint64_t key = PackTooltipKey(state_.file_explorer.GetGeneration(), item);
        return MakeTooltip(current, TooltipTarget::Zone::FilePaneItem, key, [&](std::pmr::wstring& text) {
            text = entries[item].full_path;
        });
    }
    const auto& toc_entries = state_.document.doc.GetToc().GetEntries();
    if (item >= toc_entries.size()) {
        return {};
    }
    return MakeTooltip(current, TooltipTarget::Zone::TocPaneItem, item, [&](std::pmr::wstring& text) {
        string_convert::Utf8ToWide(state_.document.doc.GetNodes()[toc_entries[item].node_index].GetText(), text);
    });
}

HCURSOR App::HoverSidePane(PaneTarget target, float dip_x, float dip_y, const PaneLayout& pane_layout, PointerHover& hover) const
{
    const auto& theme = renderer_.GetTheme();
    const PaneRect& rect = pane_layout.Get(target);
    const auto k = std::to_underlying(target);
    const auto button = mendo::app_mouse::HitSidePaneHeaderButton(
        dip_x, dip_y, rect, theme.pane_header_height, SidePaneHeaderButtonsFor(state_, target));
    hover.side_button[k] = button;
    hover.side_index[k] = SidePaneItemAt(state_, target, rect, dip_y);
    hover.tooltip = BuildSidePaneTooltip(target, button, hover.side_index[k]);
    return button != PaneHeaderButton::None ? cursors_.Hand() : cursors_.Arrow();
}

void App::OnMouseHover(int px, int py)
{
    if (!IsRenderReady()) {
        return;
    }

    // OS から同一座標の WM_MOUSEMOVE が連続して届くことがあるため、
    // 完全同一座標なら後段の zone 判定・ヒットテストを全てスキップする。
    if (state_.interaction.last_hover_pos.IsRepeat(px, py)) {
        return;
    }

    // 各ゾーンは自分のホバーだけを書き、それ以外は既定値 (ホバー無し) のまま reducer に渡して解除させる。
    const auto dip = PixelToDip(px, py);
    PointerHover hover;
    HCURSOR cursor = cursors_.Arrow();
    if (dip.y < state_.window.titlebar.GetHeight()) {
        hover.titlebar = state_.window.titlebar.HitTest(dip.x, dip.y);
        hover.tooltip = BuildTitleBarTooltip(state_.interaction.tooltip.GetCurrent(), hover.titlebar, IsZoomed(hwnd_));
    }
    else {
        const auto& pane_layout = GetPaneLayout();
        switch (const auto zone = ZoneAt(dip.x, pane_layout)) {
        case PaneZone::Splitter1:
        case PaneZone::Splitter2:
            cursor = cursors_.SizeWE();
            break;
        case PaneZone::FilePane:
        case PaneZone::TocPane:
            cursor = HoverSidePane(*ToPaneTarget(zone), dip.x, dip.y, pane_layout, hover);
            break;
        case PaneZone::MdPane:
            cursor = HoverMdPane(dip.x, dip.y, px, py, pane_layout, hover);
            break;
        case PaneZone::None:
            break;
        }
    }
    SetCursor(cursor);
    Dispatch(PointerHoverAction{ std::move(hover) });
}
