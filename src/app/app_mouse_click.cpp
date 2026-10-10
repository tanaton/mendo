#include "app.h"
#include "app_mouse_helpers.h"
#include "app_state_queries.h"
#include "block_h_scroll.h"
#include "document_utils.h"
#include "i18n.h"
#include "layout_computer.h"
#include "nav.h"
#include "pane_layout.h"
#include "string_convert.h"
#include "ui_constants.h"

void App::HandleLinkClick(std::string_view url)
{
    if (url.empty()) {
        return;
    }
    auto result = ::HandleLinkClick(url);
    switch (result.type) {
    case LinkClickResult::Type::None:
        return;
    case LinkClickResult::Type::Anchor:
        Dispatch(NavigateAnchorAction{ std::move(result.target) });
        return;
    case LinkClickResult::Type::ExternalUrl: {
        std::pmr::wstring url_wide;
        string_convert::Utf8ToWide(result.target, url_wide);
        win32_host_.ShellOpen(url_wide);
        return;
    }
    }
    std::unreachable();
}

bool App::HandleTitleBarClick(float dip_x, float dip_y)
{
    if (dip_y >= state_.window.titlebar.GetHeight()) {
        return false;
    }

    switch (state_.window.titlebar.HitTest(dip_x, dip_y)) {
    case TitleBarHitZone::OpenFile:
        Dispatch(OpenFileAction{});
        break;
    case TitleBarHitZone::Help:
        Dispatch(ShowHelpAction{});
        break;
    case TitleBarHitZone::Search:
        Dispatch(OpenSearchBarAction{});
        break;
    case TitleBarHitZone::ThemeToggle:
        Dispatch(ToggleDarkModeAction{});
        break;
    case TitleBarHitZone::FileToggle:
        Dispatch(TogglePaneAction{ PaneTarget::File });
        break;
    case TitleBarHitZone::TocToggle:
        Dispatch(TogglePaneAction{ PaneTarget::Toc });
        break;
    case TitleBarHitZone::Minimize:
        ShowWindow(hwnd_, SW_MINIMIZE);
        break;
    case TitleBarHitZone::Maximize:
        ShowWindow(hwnd_, IsZoomed(hwnd_) ? SW_RESTORE : SW_MAXIMIZE);
        break;
    case TitleBarHitZone::Close:
        PostMessageW(hwnd_, WM_CLOSE, 0, 0);
        break;
    default:
        // タイトルバーのドラッグ領域などは WM_NCHITTEST で処理済み。
        break;
    }
    return true;
}

bool App::HandleSearchBarClick(float dip_x, float dip_y, const PaneLayout& pane_layout, bool is_double_click)
{
    if (!state_.search.search_state.IsVisible()) {
        return false;
    }
    const auto sbl = ComputeSearchBarLayoutForMd(pane_layout.md_rect);
    if (dip_y < sbl.bar_top) {
        return false;
    }
    switch (HitTestSearchBar(sbl, dip_x, dip_y)) {
    case SearchBarHitZone::None:
        if (!is_double_click) {
            EmitEffect(effect::SearchFocus{});
        }
        break;
    case SearchBarHitZone::Up:
        Dispatch(SearchPrevAction{});
        break;
    case SearchBarHitZone::Down:
        Dispatch(SearchNextAction{});
        break;
    case SearchBarHitZone::CaseSensitive:
        Dispatch(ToggleCaseSensitiveAction{});
        break;
    case SearchBarHitZone::Highlight:
        Dispatch(ToggleHighlightAction{});
        break;
    case SearchBarHitZone::Close:
        Dispatch(CloseSearchBarAction{});
        break;
    case SearchBarHitZone::Input: {
        const auto& query_wide = state_.search.search_bar_ctrl.GetQueryWide();
        const int pos = HitTestSearchInputPos(sbl, query_wide, dip_x);
        if (!is_double_click) {
            Dispatch(SearchInputDragStartedAction{ pos });
            break;
        }
        // 検索 EDIT は非表示で WM_LBUTTONDBLCLK を直接受けないため、
        // 自前で単語境界を計算して EM_SETSEL を発行する。
        const auto wb = FindWordBoundaries(std::wstring_view{ query_wide }, static_cast<uint32_t>(pos));
        if (wb.found) {
            EmitEffect(effect::SearchFocus{
                effect::SearchFocus::Mode::SetSelection,
                static_cast<int>(wb.start),
                static_cast<int>(wb.end),
            });
        }
        break;
    }
    default:
        std::unreachable();
    }
    return true;
}

bool App::HandleCodeBlockButtonClick(const MdPaneHitContext& hit_ctx)
{
    // コピー/ダイアグラムコピー/保存ボタンを 1 回の可視ノード走査でまとめて判定する。
    const auto btn_hit = hit_test_.CodeBlockButtonsHitTest(hit_ctx);
    const bool dark = renderer_.GetTheme().IsDark();
    if (btn_hit.copy_node >= 0) {
        clipboard_manager_.CopyCodeBlock(state_.document.doc, btn_hit.copy_node, dark);
        return true;
    }
    if (btn_hit.diagram_copy_node >= 0) {
        // 画像は表示中ビットマップと同寿命の DiagramEntry::png から取る (ボタン表示条件と一致)。
        const auto* diagram = state_.document.layout_cache.FindDiagram(btn_hit.diagram_copy_node);
        clipboard_manager_.CopyDiagramToClipboard(state_.document.doc, btn_hit.diagram_copy_node, diagram ? diagram->png : nullptr, hit_ctx.content_width, dark);
        return true;
    }
    if (btn_hit.save_node >= 0) {
        clipboard_manager_.SaveDiagramAsPng(state_.document.doc, btn_hit.save_node, hit_ctx.content_width, dark);
        return true;
    }
    return false;
}

bool App::TryStartBlockHScrollDrag(const MdPaneHitContext& hit_ctx, float dip_x)
{
    const int hover = state_.view.hovered_h_block;
    const auto geom = ResolveBlockHScrollGeometry(state_, hover);
    if (!geom.can_scroll() || !hit_test_.BlockHScrollbarHitTest(hit_ctx, hover, geom.visible_width)) {
        return false;
    }
    Dispatch(BlockHScrollDragStartedAction{ hover, dip_x });
    return true;
}

void App::HandleMdPaneClick(float dip_x, float dip_y, int px, int py, const PaneLayout& pane_layout)
{
    if (HandleSearchBarClick(dip_x, dip_y, pane_layout, false)) {
        return;
    }

    switch (hit_test_.NavButtonHitTest(dip_x, dip_y, pane_layout.md_rect)) {
    case NavButtonHover::Back:
        Dispatch(NavigateBackAction{});
        return;
    case NavButtonHover::Forward:
        Dispatch(NavigateForwardAction{});
        return;
    default:
        break;
    }

    const auto hit_ctx = BuildMdPaneHitContext(px, py, pane_layout);
    if (HandleCodeBlockButtonClick(hit_ctx)) {
        return;
    }
    if (IsOverMdScrollbar(dip_x, dip_y, pane_layout)) {
        Dispatch(MdScrollbarDragStartedAction{ dip_y });
        return;
    }
    // テキスト選択より優先する。
    if (TryStartBlockHScrollDrag(hit_ctx, dip_x)) {
        return;
    }

    const auto hit = HitTest(px, py);
    Dispatch(TextSelectionStartedAction{ hit.node_index, hit.text_pos, px, py });
}

bool App::IsOverPaneScrollbar(float dip_x, const PaneRect& rect, const PaneScrollInfo& scroll_info) noexcept
{
    const float local_x = dip_x - rect.x;
    const float hit_left = VScrollbarLeftX(rect.width) - PANE_SCROLLBAR_HIT_PADDING;
    return local_x >= hit_left && scroll_info.total_content > scroll_info.content_height;
}

void App::RefreshFilePane()
{
    state_.file_explorer.Refresh();
    renderer_.InvalidateSidePaneCache(PaneTarget::File);
    Invalidate();
}

void App::HandleFileEntryClick(const FileEntry& entry)
{
    if (entry.is_directory()) {
        Dispatch(FilePaneDirectoryClickedAction{ entry.full_path });
        return;
    }
    if (entry.is_current()) {
        return;
    }
    if (GetFileAttributesW(entry.full_path.c_str()) == INVALID_FILE_ATTRIBUTES) {
        RefreshFilePane();
        ShowToast(i18n::S().toast_file_not_found);
        return;
    }
    Dispatch(FilePaneFileClickedAction{ entry.full_path });
}

void App::HandleSidePaneClick(PaneTarget target, float dip_x, float dip_y, const PaneLayout& layout)
{
    using mendo::app_mouse::ProcessSidePaneHeaderClick;
    using mendo::app_mouse::SidePaneHeaderButtonsFor;

    const auto& theme = renderer_.GetTheme();
    const PaneRect& rect = layout.Get(target);

    const auto on_header_button = [this, target](PaneHeaderButton hit) {
        switch (hit) {
        case PaneHeaderButton::Close:
            Dispatch(TogglePaneAction{ target });
            break;
        case PaneHeaderButton::Refresh:
            RefreshFilePane();
            break;
        case PaneHeaderButton::Reveal:
            Dispatch(FilePaneRevealCurrentFileAction{});
            break;
        case PaneHeaderButton::None:
            break;
        }
    };
    if (ProcessSidePaneHeaderClick(dip_x, dip_y, rect, theme.pane_header_height, SidePaneHeaderButtonsFor(state_, target), on_header_button)) {
        return;
    }

    const auto ctx = GetSidePaneContext(state_, target);
    if (IsOverPaneScrollbar(dip_x, rect, ctx.info)) {
        Dispatch(PaneScrollbarDragStartedAction{ target, dip_y });
        return;
    }
    const int idx = SidePaneHitTest(state_, target, SidePaneLocalY(dip_y, ctx.info.content_top, ctx.scroll.scroll_y), theme.pane_item_height);
    if (idx < 0) {
        return;
    }
    if (target == PaneTarget::File) {
        HandleFileEntryClick(state_.file_explorer.GetEntries()[idx]);
    }
    else {
        Dispatch(TocItemClickedAction{ state_.document.doc.GetToc().GetEntries()[idx].node_index });
    }
}
