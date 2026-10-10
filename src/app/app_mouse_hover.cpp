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

void App::InvalidateSidePaneAndPane(PaneTarget t)
{
    renderer_.InvalidateSidePaneCache(t);
    Invalidate();
}

void App::ResetSidePaneHover(PaneTarget t, bool reset_hover_index)
{
    bool changed = false;
    if (reset_hover_index) {
        changed |= state_.view.panes.SetHoveredSideIndex(t, -1);
    }
    changed |= state_.view.panes.ClearSideButtonHover(t);
    if (changed) {
        InvalidateSidePaneAndPane(t);
    }
}

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

TooltipTarget App::BuildMdContentTooltip(const HitResult& hit, std::optional<std::string_view> link) const
{
    if (hit.node_index < 0) {
        return {};
    }
    const auto& current = state_.interaction.tooltip.GetCurrent();
    const auto& node = state_.document.doc.GetNodes()[hit.node_index];
    const auto node_index = static_cast<uint32_t>(hit.node_index);
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

void App::HandleMdPaneHover(float dip_x, float dip_y, int px, int py, const PaneLayout& pane_layout)
{
    if (state_.search.search_state.IsVisible()) {
        const auto sbl = ComputeSearchBarLayoutForMd(pane_layout.md_rect);
        if (dip_y >= sbl.bar_top) {
            const auto zone = HitTestSearchBar(sbl, dip_x, dip_y);
            state_.search.search_bar_ctrl.UpdateHoverFromZone(zone);
            SetCursor(zone == SearchBarHitZone::Input ? cursors_.IBeam() : cursors_.Arrow());
            Dispatch(UpdateTooltipAction{ mendo::app_mouse::BuildSearchBarTooltip(state_.interaction.tooltip.GetCurrent(), zone) });
            return;
        }
        if (state_.search.search_bar_ctrl.GetHover() != SearchBarHitZone::None) {
            state_.search.search_bar_ctrl.ResetHover();
            Invalidate();
        }
    }

    if (IsOverMdScrollbar(dip_x, dip_y, pane_layout)) {
        SetCursor(cursors_.Arrow());
        Dispatch(UpdateTooltipAction{ TooltipTarget{} });
        return;
    }

    const auto nav_hit = hit_test_.NavButtonHitTest(dip_x, dip_y, pane_layout.md_rect);
    Dispatch(MdPaneNavHoverAction{ nav_hit });
    if (nav_hit != NavButtonHover::None) {
        SetCursor(cursors_.Hand());
        Dispatch(UpdateTooltipAction{ mendo::app_mouse::BuildNavButtonTooltip(state_.interaction.tooltip.GetCurrent(), nav_hit) });
        return;
    }

    // 1 回の可視ノード走査でコピー/保存/ダイアグラムコピーボタンのホバーを同時に判定する。
    const auto hit_ctx = BuildMdPaneHitContext(px, py, pane_layout);
    const auto btn_hit = hit_test_.CodeBlockButtonsHitTest(hit_ctx);
    const HoveredButtons new_hover{ btn_hit.copy_node, btn_hit.save_node, btn_hit.diagram_copy_node };
    if (new_hover != state_.interaction.hovered) {
        Dispatch(MdPaneButtonHoverChangedAction{ new_hover });
    }

    const auto emit_button_hover = [&](TooltipTarget::Zone zone, std::wstring_view text) {
        SetCursor(cursors_.Hand());
        Dispatch(UpdateTooltipAction{ MakeTooltip(state_.interaction.tooltip.GetCurrent(), zone, 0, text) });
    };
    if (new_hover.copy >= 0) {
        emit_button_hover(TooltipTarget::Zone::CopyButton, i18n::S().tooltip_copy);
        return;
    }
    if (new_hover.diagram_copy >= 0) {
        emit_button_hover(TooltipTarget::Zone::DiagramCopyButton, i18n::S().tooltip_copy_diagram);
        return;
    }
    if (new_hover.save >= 0) {
        emit_button_hover(TooltipTarget::Zone::SaveButton, i18n::S().tooltip_save_image);
        return;
    }

    const auto hit = HitTest(hit_ctx);
    const auto link = GetLinkAtHit(hit);

    // 横スクロール対象 (Table / CodeBlock) で自然幅 > 可視幅 のときだけバーを出す。
    // ドラッグ中は hovered を固定して、スクロールバー直下に出ても見た目が動かないようにする。
    if (state_.view.h_drag_node < 0) {
        const int new_h_block =
            ResolveBlockHScrollGeometry(state_, hit.node_index).can_scroll() ? hit.node_index : -1;
        if (new_h_block != state_.view.hovered_h_block) {
            Dispatch(BlockHHoverChangedAction{ new_h_block });
        }
    }

    Dispatch(UpdateTooltipAction{ BuildMdContentTooltip(hit, link) });
    SetCursor(link ? cursors_.Hand() : cursors_.IBeam());
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

int App::HandleSidePaneHover(PaneTarget target, float dip_x, float dip_y, const PaneLayout& pane_layout)
{
    const auto& theme = renderer_.GetTheme();
    // clang-format off
    auto hr = mendo::app_mouse::ProcessSidePaneHover(
        dip_x,
        dip_y,
        pane_layout.Get(target),
        theme.pane_header_height,
        theme.pane_item_height,
        mendo::app_mouse::SidePaneHeaderButtonsFor(state_, target),
        state_.view.panes.SidePaneScroll(target).scroll_y,
        [this, target](PaneHeaderButton hit) noexcept {
            return state_.view.panes.SetSideHoveredButton(target, hit);
        },
        [this, target](float y, float h) {
            return SidePaneHitTest(state_, target, y, h);
        },
        [this, target](PaneHeaderButton hit, int idx) {
            return BuildSidePaneTooltip(target, hit, idx);
        }
    );
    // clang-format on
    SetCursor(hr.any_button_hit ? cursors_.Hand() : cursors_.Arrow());
    if (hr.button_changed) {
        InvalidateSidePaneAndPane(target);
    }
    Dispatch(UpdateTooltipAction{ std::move(hr.tooltip) });
    return hr.hovered_index;
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

    const auto dip = PixelToDip(px, py);
    constexpr PaneTarget kSidePanes[] = { PaneTarget::File, PaneTarget::Toc };

    if (dip.y < state_.window.titlebar.GetHeight()) {
        const auto tb_zone = state_.window.titlebar.HitTest(dip.x, dip.y);
        SetCursor(cursors_.Arrow());
        if (state_.window.titlebar.SetHovered(tb_zone)) {
            Invalidate();
        }
        // サイドペインから直接タイトルバーへ移動すると後段のホバー解除に到達しない
        for (const auto t : kSidePanes) {
            ResetSidePaneHover(t, true);
        }
        Dispatch(UpdateTooltipAction{ mendo::app_mouse::BuildTitleBarTooltip(state_.interaction.tooltip.GetCurrent(), tb_zone, IsZoomed(hwnd_)) });
        return;
    }
    if (state_.window.titlebar.SetHovered(TitleBarHitZone::None)) {
        Invalidate();
    }

    const auto pane_layout = GetPaneLayout();
    const auto zone = ZoneAt(dip.x, pane_layout);
    const auto hovered_target = ToPaneTarget(zone);

    // ペインゾーン外に出たらヘッダーボタンのホバーをリセット（無効化忘れ防止）。
    for (const auto t : kSidePanes) {
        if (hovered_target != t) {
            ResetSidePaneHover(t, false);
        }
    }

    int new_hover[2] = { -1, -1 };

    switch (zone) {
    case PaneZone::Splitter1:
    case PaneZone::Splitter2:
        SetCursor(cursors_.SizeWE());
        Dispatch(UpdateTooltipAction{ TooltipTarget{} });
        break;
    case PaneZone::FilePane:
    case PaneZone::TocPane:
        new_hover[std::to_underlying(*hovered_target)] = HandleSidePaneHover(*hovered_target, dip.x, dip.y, pane_layout);
        break;
    case PaneZone::MdPane:
        HandleMdPaneHover(dip.x, dip.y, px, py, pane_layout);
        break;
    default:
        SetCursor(cursors_.Arrow());
        Dispatch(UpdateTooltipAction{ TooltipTarget{} });
        break;
    }

    for (const auto t : kSidePanes) {
        if (state_.view.panes.SetHoveredSideIndex(t, new_hover[std::to_underlying(t)])) {
            InvalidateSidePaneAndPane(t);
        }
    }
}
