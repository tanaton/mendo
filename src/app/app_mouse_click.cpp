#include "app.h"
#include "app_mouse_helpers.h"
#include "app_state_queries.h"
#include "block_h_scroll.h"
#include "document_utils.h"
#include "i18n.h"
#include "nav.h"
#include "pane_layout.h"
#include "string_convert.h"
#include "ui_constants.h"

namespace {

bool IsOverPaneScrollbar(float dip_x, const PaneRect& rect, const PaneScrollInfo& scroll_info) noexcept
{
    const float local_x = dip_x - rect.x;
    const float hit_left = VScrollbarLeftX(rect.width) - PANE_SCROLLBAR_HIT_PADDING;
    return local_x >= hit_left && scroll_info.total_content > scroll_info.content_height;
}

} // namespace

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
    const auto bar = SearchBarLayoutAt(dip_y, pane_layout.md_rect);
    if (!bar) {
        return false;
    }
    const SearchBarLayout& sbl = *bar;
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

void App::HandleOverlayButtonClick(const hit_test::CodeBlockButtonHit& btn_hit, float content_width)
{
    const bool dark = renderer_.GetTheme().IsDark();
    if (btn_hit.copy_node >= 0) {
        clipboard_manager_.CopyCodeBlock(state_.document.doc, btn_hit.copy_node, dark);
    }
    else if (btn_hit.diagram_copy_node >= 0) {
        // 画像は表示中ビットマップと同寿命の DiagramEntry::png から取る (ボタン表示条件と一致)。
        const auto* diagram = state_.document.layout_cache.FindDiagram(btn_hit.diagram_copy_node);
        clipboard_manager_.CopyDiagramToClipboard(state_.document.doc, btn_hit.diagram_copy_node, diagram ? diagram->png : nullptr, content_width, dark);
    }
    else if (btn_hit.save_node >= 0) {
        clipboard_manager_.SaveDiagramAsPng(state_.document.doc, btn_hit.save_node, content_width, dark);
    }
}

bool App::TryStartBlockHScrollDrag(const MdPaneHitContext& hit_ctx, float dip_x)
{
    const int hover = state_.view.hovered_h_block;
    const auto geom = ResolveBlockHScrollGeometry(state_, hover);
    if (!geom.can_scroll() || !hit_test::BlockHScrollbarHitTest(hit_ctx, hover, geom.visible_width)) {
        return false;
    }
    Dispatch(BlockHScrollDragStartedAction{ hover, dip_x });
    return true;
}

void App::HandleMdPaneClick(float dip_x, float dip_y, int px, int py, const PaneLayout& pane_layout)
{
    const auto hit_ctx = BuildMdPaneHitContext(px, py, pane_layout);
    const auto target = ResolveMdPaneHit(dip_x, dip_y, hit_ctx, pane_layout);
    switch (target.kind) {
    case MdPaneHit::Kind::SearchBar:
        HandleSearchBarClick(dip_x, dip_y, pane_layout, false);
        return;
    case MdPaneHit::Kind::NavButton:
        if (target.nav == NavButtonHover::Back) {
            Dispatch(NavigateBackAction{});
        }
        else {
            Dispatch(NavigateForwardAction{});
        }
        return;
    case MdPaneHit::Kind::OverlayButton:
        HandleOverlayButtonClick(target.buttons, hit_ctx.content_width);
        return;
    case MdPaneHit::Kind::Scrollbar:
        Dispatch(MdScrollbarDragStartedAction{ dip_y });
        return;
    case MdPaneHit::Kind::Content:
        break;
    }
    // テキスト選択より優先する。
    if (TryStartBlockHScrollDrag(hit_ctx, dip_x)) {
        return;
    }

    const auto hit = HitTest(px, py);
    Dispatch(TextSelectionStartedAction{ hit.node_index, hit.text_pos, px, py });
}

void App::RefreshFilePane()
{
    state_.file_explorer.Refresh();
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
    const auto& theme = renderer_.GetTheme();
    const PaneRect& rect = layout.Get(target);

    // ヘッダー領域内のクリックはボタン外でも消費する。
    if (dip_y - rect.y < theme.pane_header_height) {
        const auto buttons = mendo::app_mouse::SidePaneHeaderButtonsFor(state_, target);
        switch (mendo::app_mouse::HitSidePaneHeaderButton(dip_x, dip_y, rect, theme.pane_header_height, buttons)) {
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
        return;
    }

    const auto ctx = GetSidePaneContext(state_, target);
    if (IsOverPaneScrollbar(dip_x, rect, ctx.info)) {
        Dispatch(PaneScrollbarDragStartedAction{ target, dip_y });
        return;
    }
    const int idx = SidePaneItemAt(state_, target, rect, dip_y);
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
