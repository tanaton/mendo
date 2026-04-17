// MDペインのクリック・ホバー処理とヒットテスト補助。
#include "app.h"
#include "app_constants.h"
#include "app_events.h"
#include "document_utils.h"
#include "i18n.h"
#include "pane_layout.h"
#include "ui_constants.h"

// ============================================================
// ヒットテスト
// ============================================================

App::HitResult App::HitTest(int screen_x, int screen_y)
{
    const auto& layout = GetPaneLayout();
    return state_.hit_test.HitTest({
        state_.document.doc.GetNodes(), state_.document.layout_cache, renderer_.GetTheme(),
        state_.view.viewport.GetScrollY(), layout.md_rect.x,
        state_.window.cached_dpi_scale, screen_x, screen_y
        });
}

std::optional<std::pmr::wstring> App::GetLinkAtHit(const HitResult& hit) const
{
    if (hit.node_index < 0 || hit.node_index >= static_cast<int>(state_.document.doc.GetNodes().size())) {
        return std::nullopt;
    }

    return FindLinkAtPosition(state_.document.doc.GetNodes()[hit.node_index], hit.text_pos);
}

// ============================================================
// MDペイン — クリック
// ============================================================

void App::HandleMdPaneClick(float dip_x, float dip_y, int px, int py, const PaneLayout& pane_layout)
{
    // 検索バーのクリック処理
    if (state_.search.search_state.IsVisible()) {
        const auto& r = pane_layout.md_rect;
        const auto sbl = ComputeSearchBarLayout(r.x, r.width, r.y + r.height, !state_.search.search_state.GetQuery().empty());
        if (dip_y >= sbl.bar_top) {
            if (PointInRect(dip_x, dip_y, sbl.up_btn)) {
                OnSearchPrev();
                return;
            }
            if (PointInRect(dip_x, dip_y, sbl.down_btn)) {
                OnSearchNext();
                return;
            }
            if (PointInRect(dip_x, dip_y, sbl.case_btn)) {
                OnToggleCaseSensitive();
                return;
            }
            if (PointInRect(dip_x, dip_y, sbl.highlight_btn)) {
                OnToggleHighlight();
                return;
            }
            if (PointInRect(dip_x, dip_y, sbl.close_btn)) {
                OnSearchClose();
                return;
            }
            if (PointInRect(dip_x, dip_y, sbl.input_rect)) {
                const float text_left = sbl.input_rect.left + SEARCH_INPUT_TEXT_PAD_LEFT;
                const float input_w = sbl.input_rect.right - SEARCH_INPUT_TEXT_PAD_RIGHT - text_left;
                const int pos = renderer_.HitTestSearchInput(state_.search.search_state.GetQuery(), dip_x - text_left, input_w);
                state_.search.search_bar_ctrl.StartDrag(pos);
                SetCapture(hwnd_);
                PostMessage(hwnd_, app_msg::SEARCH_FOCUS, app_param::SEARCH_FOCUS_SET_CARET, static_cast<LPARAM>(pos));
                return;
            }
            PostMessage(hwnd_, app_msg::SEARCH_FOCUS, app_param::SEARCH_FOCUS_SELECT_ALL, 0);
            return;
        }
    }

    const auto nav_hit = state_.hit_test.NavButtonHitTest(dip_x, dip_y, pane_layout.md_rect);
    if (nav_hit == NavButtonHover::Back) {
        Dispatch(NavigateBackAction{});
        return;
    }
    if (nav_hit == NavButtonHover::Forward) {
        Dispatch(NavigateForwardAction{});
        return;
    }
    // コピーボタンのクリック判定（クリック位置で再判定）
    const float content_width = renderer_.GetTheme().ContentWidth(pane_layout.md_rect.width);
    const MdPaneHitContext hit_ctx{
        state_.document.doc.GetNodes(), state_.document.layout_cache, renderer_.GetTheme(),
        state_.view.viewport.GetScrollY(), pane_layout.md_rect.x,
        state_.window.cached_dpi_scale, px, py,
        content_width, pane_layout.md_rect.height
    };
    const auto copy_node = state_.hit_test.CopyButtonHitTest(hit_ctx);
    if (copy_node >= 0) {
        CopyCodeBlockToClipboard(copy_node);
        return;
    }
    // 保存ボタンのクリック判定
    const auto save_node = state_.hit_test.SaveButtonHitTest(hit_ctx);
    if (save_node >= 0) {
        SaveDiagramAsPng(save_node);
        return;
    }
    // MDペインスクロールバーのクリック判定
    if (layout_service_) {
        float total_h = layout_service_->GetTotalHeight();
        float viewport_h = pane_layout.md_rect.height;
        if (total_h > viewport_h) {
            const float sb_left = pane_layout.md_rect.x + pane_layout.md_rect.width - PANE_SCROLLBAR_WIDTH - PANE_SCROLLBAR_MARGIN;
            if (dip_x >= sb_left - PANE_SCROLLBAR_HIT_PADDING) {
                SetCapture(hwnd_);
                state_.view.panes.StartDrag(PaneController::DragTarget::MdScrollbar);
                state_.view.viewport.SetScrollbarTracking(true);
                const auto info = ComputeScrollInfo(pane_layout.md_rect, 0.0f, total_h);
                const float thumb_y = ComputeThumbY(info, state_.view.viewport.GetScrollY());
                if (dip_y >= thumb_y && dip_y <= thumb_y + info.thumb_height) {
                    state_.view.panes.SetDragScrollOffset(dip_y - thumb_y);
                }
                else {
                    state_.view.panes.SetDragScrollOffset(info.thumb_height * 0.5f);
                    const float new_thumb_y = dip_y - state_.view.panes.GetDragScrollOffset();
                    ScrollTo(ScrollFromThumbY(info, new_thumb_y));
                    Invalidate();
                }
                return;
            }
        }
    }

    // MDペイン: 選択ロジック
    SetCapture(hwnd_);
    state_.view.viewport.SetClickStart(px, py);
    auto hit = HitTest(px, py);
    if (hit.node_index >= 0) {
        state_.view.viewport.SetAnchor(hit.node_index, hit.text_pos);
        state_.view.viewport.SetDragging(true);
        state_.view.viewport.GetSelectionMut().Clear();
        InvalidateMdPane(pane_layout.md_rect);
    }
}

// ============================================================
// MDペイン — ホバー
// ============================================================

void App::HandleMdPaneHover(float dip_x, float dip_y, int px, int py, const PaneLayout& pane_layout)
{
    // 検索バー上のホバー処理
    if (state_.search.search_state.IsVisible()) {
        const auto& r = pane_layout.md_rect;
        const auto sbl = ComputeSearchBarLayout(r.x, r.width, r.y + r.height, !state_.search.search_state.GetQuery().empty());
        const auto old_hover = state_.search.search_bar_ctrl.GetHover();

        if (dip_y >= sbl.bar_top) {
            const auto hover = state_.search.search_bar_ctrl.UpdateHover(dip_x, dip_y, sbl);

            if (PointInRect(dip_x, dip_y, sbl.input_rect)) {
                SetCursor(cursors_.IBeam());
            }
            else {
                SetCursor(cursors_.Arrow());
            }
            // 検索バーボタンのツールチップ
            {
                using HZ = SearchBarController::HoverZone;
                const auto& ls = i18n::S();
                TooltipTarget tt;
                switch (hover) {
                case HZ::Up:
                    tt = { TooltipTarget::Zone::SearchBarButton, ls.tooltip_search_prev };
                    break;
                case HZ::Down:
                    tt = { TooltipTarget::Zone::SearchBarButton, ls.tooltip_search_next };
                    break;
                case HZ::CaseSensitive:
                    tt = { TooltipTarget::Zone::SearchBarButton, ls.tooltip_search_case };
                    break;
                case HZ::Highlight:
                    tt = { TooltipTarget::Zone::SearchBarButton, ls.tooltip_search_highlight };
                    break;
                case HZ::Close:
                    tt = { TooltipTarget::Zone::SearchBarButton, ls.tooltip_search_close };
                    break;
                default:
                    break;
                }
                UpdateTooltip(tt, px, py);
            }
            return;
        }

        if (old_hover != SearchBarController::HoverZone::None) {
            state_.search.search_bar_ctrl.ResetHover();
            Invalidate();
        }
    }

    // スクロールバー領域では矢印カーソル
    if (IsOverMdScrollbar(dip_x, dip_y, pane_layout)) {
        SetCursor(cursors_.Arrow());
        UpdateTooltip({}, px, py);
        return;
    }

    const auto nav_hit = state_.hit_test.NavButtonHitTest(dip_x, dip_y, pane_layout.md_rect);
    const auto old_nav_hover = state_.interaction.nav_hover;
    state_.interaction.nav_hover = nav_hit;
    if (nav_hit != NavButtonHover::None) {
        state_.interaction.hovered_copy_node = -1;
        state_.interaction.hovered_save_node = -1;
        SetCursor(cursors_.Hand());
        if (nav_hit != old_nav_hover) {
            InvalidateMdPane(pane_layout.md_rect);
        }
        // ナビゲーションボタンのツールチップ
        {
            TooltipTarget tt;
            tt.zone = TooltipTarget::Zone::NavButton;
            if (nav_hit == NavButtonHover::Back) {
                tt.text = i18n::S().tooltip_nav_back;
            }
            else {
                tt.text = i18n::S().tooltip_nav_forward;
            }
            UpdateTooltip(tt, px, py);
        }
        return;
    }
    if (old_nav_hover != NavButtonHover::None) {
        InvalidateMdPane(pane_layout.md_rect);
    }

    // コピー/保存ボタンのホバー判定（距離スロットリングで不要な再計算を回避）
    const float content_width = renderer_.GetTheme().ContentWidth(pane_layout.md_rect.width);
    const MdPaneHitContext hit_ctx{
        state_.document.doc.GetNodes(), state_.document.layout_cache, renderer_.GetTheme(),
        state_.view.viewport.GetScrollY(), pane_layout.md_rect.x,
        state_.window.cached_dpi_scale, px, py,
        content_width, pane_layout.md_rect.height
    };
    {
        const int cdx = px - state_.interaction.hover_throttle.last_copy_hit_pos.x;
        const int cdy = py - state_.interaction.hover_throttle.last_copy_hit_pos.y;
        if (cdx * cdx + cdy * cdy > HOVER_THROTTLE_DISTANCE_SQ) {
            state_.interaction.hover_throttle.last_copy_hit_pos = { px, py };
            const int old_copy_hover = state_.interaction.hovered_copy_node;
            state_.interaction.hovered_copy_node = state_.hit_test.CopyButtonHitTest(hit_ctx);
            if (state_.interaction.hovered_copy_node != old_copy_hover) {
                InvalidateMdPane(pane_layout.md_rect);
            }
        }
    }
    if (state_.interaction.hovered_copy_node >= 0) {
        SetCursor(cursors_.Hand());
        UpdateTooltip({ TooltipTarget::Zone::CopyButton, i18n::S().tooltip_copy }, px, py);
        return;
    }

    {
        const int sdx = px - state_.interaction.hover_throttle.last_save_hit_pos.x;
        const int sdy = py - state_.interaction.hover_throttle.last_save_hit_pos.y;
        if (sdx * sdx + sdy * sdy > HOVER_THROTTLE_DISTANCE_SQ) {
            state_.interaction.hover_throttle.last_save_hit_pos = { px, py };
            const int old_save_hover = state_.interaction.hovered_save_node;
            state_.interaction.hovered_save_node = state_.hit_test.SaveButtonHitTest(hit_ctx);
            if (state_.interaction.hovered_save_node != old_save_hover) {
                InvalidateMdPane(pane_layout.md_rect);
            }
        }
    }
    if (state_.interaction.hovered_save_node >= 0) {
        SetCursor(cursors_.Hand());
        UpdateTooltip({ TooltipTarget::Zone::SaveButton, i18n::S().tooltip_save_image }, px, py);
        return;
    }

    // リンク・画像のヒットテスト
    const int dx = px - state_.interaction.hover_throttle.last_md_hit_pos.x;
    const int dy = py - state_.interaction.hover_throttle.last_md_hit_pos.y;
    if (dx * dx + dy * dy > HOVER_THROTTLE_DISTANCE_SQ) {
        const auto hit = HitTest(px, py);
        const auto link = GetLinkAtHit(hit);
        state_.interaction.hover_throttle.last_md_cursor_hand = link.has_value();
        state_.interaction.hover_throttle.last_md_hit_pos = { px, py };

        // リンクまたは画像のツールチップ
        TooltipTarget tt;
        if (link.has_value()) {
            tt.zone = TooltipTarget::Zone::MdLink;
            tt.text = *link;
        }
        else if (hit.node_index >= 0) {
            const auto& nodes = state_.document.doc.GetNodes();
            const auto& node = nodes[hit.node_index];
            if (node.type == NodeType::Image && node.has_image()) {
                tt.zone = TooltipTarget::Zone::MdImage;
                const auto& alt = node.GetText();
                if (!alt.empty()) {
                    tt.text = alt;
                    tt.text += L"\n";
                }
                tt.text += node.image_data->src;
            }
        }
        UpdateTooltip(tt, px, py);
    }
    SetCursor(state_.interaction.hover_throttle.last_md_cursor_hand ? cursors_.Hand() : cursors_.IBeam());
}

// ============================================================
// MDペイン — スクロールバー判定
// ============================================================

bool App::IsOverMdScrollbar(float dip_x, float dip_y, const PaneLayout& layout) const noexcept
{
    if (!layout_service_) {
        return false;
    }
    const float total_h = layout_service_->GetTotalHeight();
    const float viewport_h = layout.md_rect.height;
    if (total_h <= viewport_h || viewport_h <= 0.0f) {
        return false;
    }
    if (dip_y < layout.md_rect.y || dip_y > layout.md_rect.y + viewport_h) {
        return false;
    }
    const float md_right = layout.md_rect.x + layout.md_rect.width;
    const float sb_left = md_right - PANE_SCROLLBAR_WIDTH - PANE_SCROLLBAR_MARGIN;
    const float sb_right = md_right - PANE_SCROLLBAR_MARGIN;
    return dip_x >= sb_left - PANE_SCROLLBAR_HIT_PADDING && dip_x <= sb_right;
}

bool App::IsOverMdScrollbar(float dip_x, float dip_y)
{
    return IsOverMdScrollbar(dip_x, dip_y, GetPaneLayout());
}
