#include "app.h"
#include "block_h_scroll.h"
#include "document_utils.h"
#include "i18n.h"
#include "nav.h"
#include "pane_layout.h"
#include "selection_html.h"
#include "string_convert.h"
#include "titlebar.h"
#include "tooltip.h"
#include "ui_constants.h"
#include "ui_types.h"
#include "win32_util.h"
#include <string_view>
#include <utility>

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

} // namespace

App::HitResult App::HitTest(int px, int py)
{
    return hit_test::HitTest(BuildMdPaneHitContext(px, py, GetPaneLayout()));
}

MdPaneHitContext App::BuildMdPaneHitContext(int px, int py, const PaneLayout& pane_layout) const noexcept
{
    const auto& theme = theme_service_.GetTheme();
    return MdPaneHitContext{
        .nodes = state_.document.doc.GetNodes(),
        .cache = state_.document.layout_cache,
        .theme = theme,
        .scroll_y = state_.view.viewport.GetScrollY(),
        .md_rect = pane_layout.md_rect,
        .dpi_scale = state_.window.cached_dpi_scale,
        .screen_x = px,
        .screen_y = py,
        .content_width = theme.ContentWidth(pane_layout.md_rect.width),
        .block_scroll_x = &state_.view.block_scroll_x,
    };
}

std::optional<LinkHit> App::GetLinkAtHit(const HitResult& hit) const
{
    const auto& nodes = state_.document.doc.GetNodes();
    if (hit.node_index < 0 || hit.node_index >= static_cast<int>(nodes.size())) {
        return std::nullopt;
    }
    return FindLinkHitAtPosition(nodes[hit.node_index], hit.char_pos);
}

void App::OnLButtonDown(int px, int py)
{
    const auto dip = PixelToDip(px, py);

    if (HandleTitleBarClick(dip.x, dip.y)) {
        return;
    }

    const auto pane_layout = GetPaneLayout();
    const auto zone = ZoneAt(dip.x, pane_layout);

    switch (zone) {
    case PaneZone::None:
        return;
    case PaneZone::FilePane:
    case PaneZone::TocPane:
        HandleSidePaneClick(*ToPaneTarget(zone), dip.x, dip.y, pane_layout);
        return;
    case PaneZone::Splitter1:
        Dispatch(SplitterDragStartedAction{ PaneTarget::File });
        return;
    case PaneZone::Splitter2:
        Dispatch(SplitterDragStartedAction{ PaneTarget::Toc });
        return;
    case PaneZone::MdPane:
        HandleMdPaneClick(dip.x, dip.y, px, py, pane_layout, false);
        return;
    }
    std::unreachable();
}

void App::OnLButtonUp(int px, int py)
{
    const LeftDrag drag = state_.view.drag.kind;
    if (drag == LeftDrag::None) {
        return;
    }
    if (drag != LeftDrag::TextSelect) {
        Dispatch(LeftDragEndedAction{});
        return;
    }
    const auto hit = HitTest(px, py);
    const int dx = px - state_.view.drag.click_x;
    const int dy = py - state_.view.drag.click_y;
    const bool small_click = (dx * dx + dy * dy) < CLICK_DISTANCE_THRESHOLD_SQ;
    Dispatch(TextSelectionEndedAction{ hit.node_index, hit.text_pos });
    if (!small_click || state_.view.viewport.GetSelection().active()) {
        return;
    }
    if (const auto link = GetLinkAtHit(hit)) {
        HandleLinkClick(link->url);
    }
}

void App::OnLButtonDblClk(int px, int py)
{
    const auto dip = PixelToDip(px, py);
    // CS_DBLCLKS により連続クリックの2回目は WM_LBUTTONDBLCLK になるため、ボタンやスクロールバーは
    // 1 回目と同じ判定で連打として扱い、本文の上でだけ単語を選ぶ。
    if (HandleTitleBarClick(dip.x, dip.y)) {
        return;
    }
    // サイドペインは 1 回目のクリックで一覧が変わりうる (フォルダに入る) ので、2 回目を別の項目に当てない。
    const auto pane_layout = GetPaneLayout();
    if (ZoneAt(dip.x, pane_layout) == PaneZone::MdPane) {
        HandleMdPaneClick(dip.x, dip.y, px, py, pane_layout, true);
    }
}

void App::OnMouseMove(int px, int py)
{
    const auto dip = PixelToDip(px, py);

    switch (state_.view.drag.kind) {
    case LeftDrag::None:
        return;
    case LeftDrag::SearchInput: {
        const auto sbl = ComputeSearchBarLayoutForMd(GetPaneLayout().md_rect);
        Dispatch(SearchInputDragMovedAction{ HitTestSearchInputPos(sbl, dip.x) });
        return;
    }
    case LeftDrag::Splitter:
        Dispatch(SplitterDragMovedAction{ dip.x });
        return;
    case LeftDrag::PaneScrollbar:
        Dispatch(PaneScrollbarDragMovedAction{ dip.y });
        return;
    case LeftDrag::MdScrollbar:
        Dispatch(MdScrollbarDragMovedAction{ dip.y });
        return;
    case LeftDrag::BlockHScroll:
        Dispatch(BlockHScrollDragMovedAction{ dip.x });
        return;
    case LeftDrag::TextSelect:
        if (const auto hit = HitTest(px, py); hit.node_index >= 0) {
            Dispatch(TextSelectionMovedAction{ hit.node_index, hit.text_pos });
        }
        return;
    }
    std::unreachable();
}

void App::OnMouseWheel(int px, int py, int delta, bool ctrl)
{
    const auto dip = PixelToDip(px, py);
    Dispatch(MouseWheelAction{ delta, ctrl, PaneAtPoint(dip.x), GetTickCount64() });
}

bool App::OnRButtonDown(int px, int py)
{
    // 左ドラッグ進行中にジェスチャを開始すると、完了時の ReleaseCapture が
    // 進行中ドラッグのキャプチャを破壊する。
    if (state_.view.drag.kind != LeftDrag::None) {
        return false;
    }
    const auto dip = PixelToDip(px, py);
    if (PaneAtPoint(dip.x) != PaneZone::MdPane) {
        return false;
    }
    Dispatch(RightClickGestureStartedAction{ dip.x, dip.y });
    return true;
}

bool App::OnRButtonUp(int px, int py)
{
    if (state_.interaction.gesture.GetPhase() == GesturePhase::Idle) {
        return false;
    }
    POINT pt{ px, py };
    ClientToScreen(hwnd_, &pt);
    Dispatch(RightClickGestureCompletedAction{ pt.x, pt.y });
    return true;
}

void App::OnRButtonMove(int px, int py)
{
    const auto dip = PixelToDip(px, py);
    Dispatch(RightClickGestureMovedAction{ dip.x, dip.y });
}

void App::HandleLinkClick(std::string_view url)
{
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
        ShellOpenAsync(std::move(url_wide));
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
    case TitleBarHitZone::Settings:
        ShowSettingsDialog();
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

void App::HandleSearchBarClick(SearchBarHitZone zone, float dip_x, const PaneRect& md_rect, bool is_double_click)
{
    switch (zone) {
    case SearchBarHitZone::None:
        if (!is_double_click) {
            window_.FocusSearchEdit(0, -1);
        }
        return;
    case SearchBarHitZone::Up:
        Dispatch(SearchPrevAction{});
        return;
    case SearchBarHitZone::Down:
        Dispatch(SearchNextAction{});
        return;
    case SearchBarHitZone::CaseSensitive:
        Dispatch(ToggleCaseSensitiveAction{});
        return;
    case SearchBarHitZone::Highlight:
        Dispatch(ToggleHighlightAction{});
        return;
    case SearchBarHitZone::Close:
        Dispatch(CloseSearchBarAction{});
        return;
    case SearchBarHitZone::Input: {
        const int pos = HitTestSearchInputPos(ComputeSearchBarLayoutForMd(md_rect), dip_x);
        if (!is_double_click) {
            Dispatch(SearchInputDragStartedAction{ pos });
            return;
        }
        // 検索 EDIT は非表示で WM_LBUTTONDBLCLK を直接受けないため、
        // 自前で単語境界を計算して EM_SETSEL を発行する。
        const auto wb = FindWordBoundaries(std::wstring_view{ state_.search.query_wide }, static_cast<uint32_t>(pos));
        if (wb.found) {
            window_.FocusSearchEdit(static_cast<int>(wb.start), static_cast<int>(wb.end));
        }
        return;
    }
    }
    std::unreachable();
}

void App::HandleOverlayButtonClick(const OverlayButtonHit& hit)
{
    const bool dark = theme_service_.GetTheme().dark;
    switch (hit.kind) {
    case OverlayButton::CodeCopy:
        clipboard_manager_.CopyCodeBlock(state_.document.doc, hit.node, dark);
        return;
    case OverlayButton::DiagramCopy:
    case OverlayButton::DiagramSave: {
        // ボタンは DiagramEntry::png があるときだけ出す。
        const auto* diagram = state_.document.layout_cache.FindDiagram(hit.node);
        const PngBytes png = diagram ? diagram->png : nullptr;
        if (hit.kind == OverlayButton::DiagramCopy) {
            clipboard_manager_.CopyDiagramToClipboard(state_.document.doc, hit.node, png, MdContentWidth(), dark);
        }
        else {
            clipboard_manager_.SaveDiagramAsPng(state_.document.doc, hit.node, png);
        }
        return;
    }
    case OverlayButton::None:
        return;
    }
    std::unreachable();
}

bool App::TryStartBlockHScrollDrag(const MdPaneHitContext& hit_ctx, float dip_x)
{
    const int hover = state_.view.hover.h_block;
    const auto geom = ResolveBlockHScrollGeometry(state_, hover);
    if (!geom.can_scroll() || !hit_test::BlockHScrollbarHitTest(hit_ctx, hover, geom.visible_width)) {
        return false;
    }
    Dispatch(BlockHScrollDragStartedAction{ hover, dip_x });
    return true;
}

void App::HandleMdPaneClick(float dip_x, float dip_y, int px, int py, const PaneLayout& pane_layout, bool is_double_click)
{
    const auto hit_ctx = BuildMdPaneHitContext(px, py, pane_layout);
    const auto target = ResolveMdPaneHit(dip_x, dip_y, hit_ctx, pane_layout);
    switch (target.kind) {
    case MdPaneHit::Kind::SearchBar:
        HandleSearchBarClick(target.search_bar, dip_x, pane_layout.md_rect, is_double_click);
        return;
    case MdPaneHit::Kind::NavButton:
        if (target.nav == NavDirection::Back) {
            Dispatch(NavigateBackAction{});
        }
        else {
            Dispatch(NavigateForwardAction{});
        }
        return;
    case MdPaneHit::Kind::OverlayButton:
        HandleOverlayButtonClick(target.overlay_button);
        return;
    case MdPaneHit::Kind::Scrollbar:
        Dispatch(MdScrollbarDragStartedAction{ dip_y });
        return;
    case MdPaneHit::Kind::Content: {
        // テキスト選択より優先する。
        if (TryStartBlockHScrollDrag(hit_ctx, dip_x)) {
            return;
        }
        const auto hit = hit_test::HitTest(hit_ctx);
        if (is_double_click) {
            // 単語はカーソル下の文字で決める。文字の上でない (行末より右等) ときはキャレット位置で決める。
            Dispatch(SelectWordAction{ hit.node_index, hit.char_pos != hit_test::HitResult::kNoChar ? hit.char_pos : hit.text_pos });
            return;
        }
        Dispatch(TextSelectionStartedAction{ hit.node_index, hit.text_pos, px, py });
        return;
    }
    }
    std::unreachable();
}

void App::RefreshFilePane()
{
    state_.file_explorer.Refresh();
    Invalidate();
}

void App::HandleFileEntryClick(const FileEntry& entry)
{
    if (entry.is_directory) {
        Dispatch(FilePaneDirectoryClickedAction{ entry.full_path });
        return;
    }
    if (entry.is_current) {
        return;
    }
    // 消えていたら読み込みの失敗 (HandleLoadFailureFallback) で知らせて一覧を直す。
    Dispatch(OpenPathAction{ entry.full_path });
}

void App::HandleSidePaneClick(PaneTarget target, float dip_x, float dip_y, const PaneLayout& layout)
{
    const auto& theme = theme_service_.GetTheme();
    const PaneRect& rect = layout.Get(target);

    // ヘッダー領域内のクリックはボタン外でも消費する。
    if (dip_y - rect.y < theme.pane_header_height) {
        const auto buttons = SidePaneHeaderButtonsFor(state_, target);
        switch (HitSidePaneHeaderButton(dip_x, dip_y, rect, theme.pane_header_height, buttons)) {
        case PaneHeaderButton::None:
            return;
        case PaneHeaderButton::Close:
            Dispatch(TogglePaneAction{ target });
            return;
        case PaneHeaderButton::Refresh:
            RefreshFilePane();
            return;
        case PaneHeaderButton::Reveal:
            Dispatch(FilePaneRevealCurrentFileAction{});
            return;
        }
        std::unreachable();
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
        Dispatch(TocItemClickedAction{ state_.document.doc.GetToc()[idx].node_index });
    }
}

bool App::IsOverMdScrollbar(float dip_x, float dip_y, const PaneLayout& layout) const noexcept
{
    const PaneRect& md = layout.md_rect;
    if (dip_y < md.y || dip_y > md.y + md.height) {
        return false;
    }
    return IsOverPaneScrollbar(dip_x, md, ComputeScrollInfo(md, 0.0f, ScrollableContentHeight()), state_.document.layout_cache.HasDirtyNodes());
}

bool App::IsOverMdScrollbar(float dip_x, float dip_y)
{
    return IsOverMdScrollbar(dip_x, dip_y, GetPaneLayout());
}

TooltipHover App::BuildMdContentTooltip(int hit_node, std::optional<LinkHit> link) const
{
    if (hit_node < 0) {
        return {};
    }
    const auto& current = state_.interaction.tooltip;
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
    if (const auto nav = hit_test::NavButtonHitTest(dip_x, dip_y, pane_layout.md_rect); nav != NavDirection::None) {
        return { .kind = Kind::NavButton, .nav = nav };
    }
    // 縮小表示で余白が狭いとボタンがスクロールバーのヒット余白に掛かるので、ボタンを優先する。
    if (const auto button = hit_test::OverlayButtonHitTest(hit_ctx); button.Any()) {
        return { .kind = Kind::OverlayButton, .overlay_button = button };
    }
    if (IsOverMdScrollbar(dip_x, dip_y, pane_layout)) {
        return { .kind = Kind::Scrollbar };
    }
    return {};
}

HCURSOR App::HoverMdPane(float dip_x, float dip_y, int px, int py, const PaneLayout& pane_layout, PointerHover& hover) const
{
    const auto& current_tooltip = state_.interaction.tooltip;
    const auto hit_ctx = BuildMdPaneHitContext(px, py, pane_layout);
    const auto target = ResolveMdPaneHit(dip_x, dip_y, hit_ctx, pane_layout);
    switch (target.kind) {
    case MdPaneHit::Kind::SearchBar:
        hover.search_bar = target.search_bar;
        hover.tooltip = MakeTooltip(current_tooltip, TooltipTarget::Zone::SearchBarButton, std::to_underlying(hover.search_bar), SearchBarTooltipText(hover.search_bar));
        return hover.search_bar == SearchBarHitZone::Input ? cursors_.ibeam : cursors_.arrow;
    case MdPaneHit::Kind::NavButton:
        hover.nav = target.nav;
        hover.tooltip = MakeTooltip(current_tooltip, TooltipTarget::Zone::NavButton, std::to_underlying(hover.nav), NavButtonTooltipText(hover.nav));
        return cursors_.hand;
    case MdPaneHit::Kind::Scrollbar:
        return cursors_.arrow;
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
    if (state_.view.HDragNode() < 0 && ResolveBlockHScrollGeometry(state_, node_index).can_scroll()) {
        hover.h_block = node_index;
    }

    hover.overlay_button = target.overlay_button;
    if (hover.overlay_button.Any()) {
        const auto kind = hover.overlay_button.kind;
        hover.tooltip = MakeTooltip(current_tooltip, TooltipTarget::Zone::OverlayButton, std::to_underlying(kind), OverlayButtonTooltipText(kind));
        return cursors_.hand;
    }

    // 文字位置はリンク判定にしか使わないので、リンクを含むノードの上でだけ求める。
    std::optional<LinkHit> link;
    if (node_index >= 0 && !state_.document.doc.GetNodes()[node_index].view_link_urls().empty()) {
        link = GetLinkAtHit(hit_test::HitTest(hit_ctx));
    }
    hover.tooltip = BuildMdContentTooltip(node_index, link);
    return link ? cursors_.hand : cursors_.ibeam;
}

TooltipHover App::BuildSidePaneTooltip(PaneTarget target, PaneHeaderButton hit, int idx) const
{
    const bool is_file = target == PaneTarget::File;
    const auto& current = state_.interaction.tooltip;
    if (hit != PaneHeaderButton::None) {
        const auto zone = is_file ? TooltipTarget::Zone::FilePaneButton : TooltipTarget::Zone::TocPaneButton;
        return MakeTooltip(current, zone, std::to_underlying(hit), PaneHeaderButtonTooltipText(hit));
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
    const auto& toc_entries = state_.document.doc.GetToc();
    if (item >= toc_entries.size()) {
        return {};
    }
    return MakeTooltip(current, TooltipTarget::Zone::TocPaneItem, item, [&](std::pmr::wstring& text) {
        string_convert::Utf8ToWide(state_.document.doc.GetNodes()[toc_entries[item].node_index].GetText(), text);
    });
}

HCURSOR App::HoverSidePane(PaneTarget target, float dip_x, float dip_y, const PaneLayout& pane_layout, PointerHover& hover) const
{
    const auto& theme = theme_service_.GetTheme();
    const PaneRect& rect = pane_layout.Get(target);
    const auto k = std::to_underlying(target);
    const auto button = HitSidePaneHeaderButton(
        dip_x, dip_y, rect, theme.pane_header_height, SidePaneHeaderButtonsFor(state_, target));
    hover.side_button[k] = button;
    hover.side_index[k] = SidePaneItemAt(state_, target, rect, dip_y);
    hover.tooltip = BuildSidePaneTooltip(target, button, hover.side_index[k]);
    return button != PaneHeaderButton::None ? cursors_.hand : cursors_.arrow;
}

void App::RefreshHoverAtCursor()
{
    // ボタンを押している間は WM_MOUSEMOVE でもホバーを引かない (ドラッグ側が扱う)。
    if (IsKeyDown(VK_LBUTTON) || IsKeyDown(VK_RBUTTON)) {
        return;
    }
    if (in_modal_loop_) {
        return;
    }
    POINT pt{};
    if (!GetCursorPos(&pt) || WindowFromPoint(pt) != hwnd_) {
        return;
    }
    // リサイズ枠やキャプションの上でカーソルを矢印に戻さない。
    if (window_.HitTestNonClient(pt) != HTCLIENT) {
        return;
    }
    ScreenToClient(hwnd_, &pt);
    OnMouseHover(pt.x, pt.y);
}

void App::OnMouseHover(int px, int py)
{
    // 各ゾーンは自分のホバーだけを書き、それ以外は既定値 (ホバー無し) のまま reducer に渡して解除させる。
    const auto dip = PixelToDip(px, py);
    PointerHover hover;
    HCURSOR cursor = cursors_.arrow;
    if (dip.y < state_.window.titlebar.GetHeight()) {
        hover.titlebar = state_.window.titlebar.HitTest(dip.x, dip.y);
        const bool maximized = IsZoomed(hwnd_);
        // 最大化ボタンは状態で文言が変わるため key を分ける。
        const uint64_t key = std::to_underlying(hover.titlebar) * 2ull + (hover.titlebar == TitleBarHitZone::Maximize && maximized);
        hover.tooltip = MakeTooltip(state_.interaction.tooltip, TooltipTarget::Zone::TitleBarButton, key, TitleBarTooltipText(hover.titlebar, maximized));
    }
    else {
        const auto& pane_layout = GetPaneLayout();
        switch (const auto zone = ZoneAt(dip.x, pane_layout)) {
        case PaneZone::Splitter1:
        case PaneZone::Splitter2:
            cursor = cursors_.size_we;
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
