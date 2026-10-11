#include "app.h"
#include "app_mouse_helpers.h"
#include "app_state_queries.h"
#include "block_h_scroll.h"
#include "i18n.h"
#include "pane_layout.h"
#include "string_convert.h"
#include "ui_constants.h"

namespace {

// 上位 32bit に親 (node index・一覧の世代)、下位に子 (リンク index・項目 index) を詰める。
constexpr uint64_t PackTooltipKey(uint32_t outer, uint32_t inner) noexcept
{
    return (uint64_t{ outer } << 32) | inner;
}

// FindLinkAtPosition は view_link_urls() の要素を指す view を返すので、アドレスで添字を引ける。
uint32_t LinkUrlIndex(const Node& node, std::string_view url) noexcept
{
    const auto urls = node.view_link_urls();
    const auto it = std::ranges::find(urls, url.data(), [](const std::pmr::string& u) noexcept { return u.data(); });
    return static_cast<uint32_t>(it - urls.begin());
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
    const float sb_left = VScrollbarLeftX(md_right);
    const float sb_right = md_right - PANE_SCROLLBAR_MARGIN;
    return dip_x >= sb_left - PANE_SCROLLBAR_HIT_PADDING && dip_x <= sb_right;
}

bool App::IsOverMdScrollbar(float dip_x, float dip_y)
{
    return IsOverMdScrollbar(dip_x, dip_y, GetPaneLayout());
}

TooltipTarget App::BuildMdContentTooltip(int hit_node, std::optional<std::string_view> link) const
{
    if (hit_node < 0) {
        return {};
    }
    const auto& current = state_.interaction.tooltip.GetCurrent();
    const auto& node = state_.document.doc.GetNodes()[hit_node];
    const auto node_index = static_cast<uint32_t>(hit_node);
    if (link) {
        const uint64_t key = PackTooltipKey(node_index, LinkUrlIndex(node, *link));
        return MakeTooltip(current, TooltipTarget::Zone::MdLink, key, [&](std::pmr::wstring& text) {
            string_convert::Utf8ToWide(*link, text);
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

HCURSOR App::HoverMdPane(float dip_x, float dip_y, int px, int py, const PaneLayout& pane_layout, PointerHover& hover) const
{
    const auto& current_tooltip = state_.interaction.tooltip.GetCurrent();
    if (state_.search.search_state.IsVisible()) {
        const auto sbl = ComputeSearchBarLayoutForMd(pane_layout.md_rect);
        if (dip_y >= sbl.bar_top) {
            hover.search_bar = HitTestSearchBar(sbl, dip_x, dip_y);
            hover.tooltip = mendo::app_mouse::BuildSearchBarTooltip(current_tooltip, hover.search_bar);
            return hover.search_bar == SearchBarHitZone::Input ? cursors_.IBeam() : cursors_.Arrow();
        }
    }

    if (IsOverMdScrollbar(dip_x, dip_y, pane_layout)) {
        return cursors_.Arrow();
    }

    hover.nav = hit_test_.NavButtonHitTest(dip_x, dip_y, pane_layout.md_rect);
    if (hover.nav != NavButtonHover::None) {
        hover.tooltip = mendo::app_mouse::BuildNavButtonTooltip(current_tooltip, hover.nav);
        return cursors_.Hand();
    }

    const auto hit_ctx = BuildMdPaneHitContext(px, py, pane_layout);
    const int node_index = hit_test_.HitNode(hit_ctx);
    // 横スクロール対象 (Table / CodeBlock) で自然幅 > 可視幅 のときだけバーを出す。
    // ドラッグ中は hovered を固定して、スクロールバー直下に出ても見た目が動かないようにする。
    if (state_.view.h_drag_node >= 0) {
        hover.h_block = state_.view.hovered_h_block;
    }
    else if (ResolveBlockHScrollGeometry(state_, node_index).can_scroll()) {
        hover.h_block = node_index;
    }

    // 1 回の可視ノード走査でコピー/保存/ダイアグラムコピーボタンのホバーを同時に判定する。
    const auto btn_hit = hit_test_.CodeBlockButtonsHitTest(hit_ctx);
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
    std::optional<std::string_view> link;
    if (node_index >= 0 && !state_.document.doc.GetNodes()[node_index].view_link_urls().empty()) {
        link = GetLinkAtHit(hit_test_.HitTest(hit_ctx));
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
        dip_x, dip_y, rect, theme.pane_header_height, mendo::app_mouse::SidePaneHeaderButtonsFor(state_, target));
    hover.side_button[k] = button;
    hover.side_index[k] = SidePaneHitTest(
        state_, target, SidePaneLocalY(dip_y, rect.y + theme.pane_header_height, state_.view.panes.SidePaneScroll(target).scroll_y), theme.pane_item_height);
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
        hover.tooltip = mendo::app_mouse::BuildTitleBarTooltip(state_.interaction.tooltip.GetCurrent(), hover.titlebar, IsZoomed(hwnd_));
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
