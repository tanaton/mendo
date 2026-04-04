#include "app.h"
#include "parser.h"
#include "resource.h"
#include "i18n.h"
#include "pane_layout.h"
#include "document_utils.h"
#include "mermaid_util.h"
#include "layout.h"
#include <windowsx.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <variant>
#include <filesystem>
#include <dwmapi.h>
#include <uxtheme.h>

#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "shcore.lib")
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "uxtheme.lib")

// DWMWA_USE_IMMERSIVE_DARK_MODE (Windows 10 1809以降 / Windows 11でサポート)
#ifndef DWMWA_USE_IMMERSIVE_DARK_MODE
#define DWMWA_USE_IMMERSIVE_DARK_MODE 20
#endif

#include "utility.h"
#include "profiler.h"

void ApplyDarkModeToWindow(HWND hwnd, bool dark)
{
    // ダークタイトルバー
    const BOOL value = dark ? TRUE : FALSE;
    DwmSetWindowAttribute(hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE, &value, sizeof(value));

    // エクスプローラーテーマによるダークスクロールバー
    SetWindowTheme(hwnd, dark ? L"DarkMode_Explorer" : L"Explorer", nullptr);
}

// ============================================================
// 初期化
// ============================================================

bool App::Init(HWND hwnd)
{
    hwnd_ = hwnd;

    if (!renderer_.Init(hwnd_)) {
        return false;
    }

    layout_service_.emplace(renderer_.GetLayout(), viewport_);

    // PixelToDip用にDPIスケールをキャッシュ (OnDpiChangedで更新)
    const float init_dpi = static_cast<float>(GetDpiForWindow(hwnd_));
    cached_dpi_scale_ = (init_dpi > 0.0f) ? (init_dpi / 96.0f) : 1.0f;

    // タスクスケジューラを初期化（画像デコード・キャッシュ書き込み共用）
    scheduler_.Init(mermaid_util::ComputeWorkerCount(
        std::thread::hardware_concurrency()));

    // Mermaidファイルキャッシュを初期化
    file_cache_.Init(cached_dpi_scale_, scheduler_);
    mermaid_renderer_.SetFileCache(&file_cache_);

    // Mermaidレンダラーを初期化 (WebView2、非同期)
    mermaid_renderer_.Init(hwnd_, renderer_.GetRenderTarget(), [this]() {
        ScheduleMermaidBatch();
    });

    // 画像ローダーを初期化（WICファクトリはバックエンドと共有）
    image_loader_.Init(renderer_.GetRenderTarget(), renderer_.GetWICFactory());
    image_loader_.InitAsync(hwnd_, WM_APP_IMAGE_LOADED, scheduler_);

    // D2Dデバイスロスト時にレンダーターゲットが再作成されたら、各ローダーを更新
    renderer_.SetDeviceLostCallback([this](ID2D1RenderTarget* new_rt) {
        mermaid_renderer_.SetRenderTarget(new_rt);
        image_loader_.CancelPending();
        image_loader_.SetRenderTarget(new_rt);
        image_loader_.ClearCache();
        LoadImages();
    });

    // 保存済みのダークモードとズーム設定を適用
    theme_service_.LoadDarkMode();
    viewport_.SetZoomIndex(theme_service_.LoadZoomIndex());
    if (theme_service_.IsDarkMode() || viewport_.GetZoomIndex() != ZOOM_DEFAULT_INDEX) {
        renderer_.SetTheme(theme_service_.CreateTheme(viewport_.GetZoomIndex()));
        if (viewport_.GetZoomIndex() != ZOOM_DEFAULT_INDEX) {
            panes_.ApplyZoom(viewport_.GetCurrentZoom());
        }
    }
    if (theme_service_.IsDarkMode()) {
        ApplyDarkModeToWindow(hwnd_, true);
    }

    // システムカーソルをキャッシュ
    cursor_arrow_ = LoadCursorW(nullptr, IDC_ARROW);
    cursor_hand_ = LoadCursorW(nullptr, IDC_HAND);
    cursor_ibeam_ = LoadCursorW(nullptr, IDC_IBEAM);
    cursor_sizewe_ = LoadCursorW(nullptr, IDC_SIZEWE);

    // タイトルバーのレイアウト初期化
    {
        const auto* rt = renderer_.GetRenderTarget();
        const float window_w = rt ? rt->GetSize().width : 1600.0f;
        titlebar_.UpdateLayout(window_w);
    }

    LoadPaneState();

    ctx_menu_.Init(renderer_.GetD2DFactory(), renderer_.GetDWriteFactory());

    // ツールチップを初期化
    tooltip_.Init(hwnd_);
    if (theme_service_.IsDarkMode()) {
        tooltip_.ApplyDarkMode(true);
    }

    // ファイル監視タイマーを設定 (250ms毎にチェック)
    SetTimer(hwnd_, TIMER_FILE_WATCH, 250, nullptr);

    return true;
}

// ============================================================
// ヘルパー
// ============================================================

App::DipPoint App::PixelToDip(int px, int py) const noexcept
{
    return { px / cached_dpi_scale_, py / cached_dpi_scale_ };
}

PaneScrollInfo App::ComputePaneScrollInfo(
    const PaneRect& rect, float total_content) const
{
    return ComputeScrollInfo(rect, renderer_.GetTheme().pane_header_height, total_content);
}

void App::HandleScrollbarClick(float dip_y, const PaneScrollInfo& info,
    ScrollState& scroll, bool& cache_dirty)
{
    const float thumb_y = ComputeThumbY(info, scroll.scroll_y);

    if (dip_y >= thumb_y && dip_y <= thumb_y + info.thumb_height) {
        panes_.SetDragScrollOffset(dip_y - thumb_y);
    }
    else {
        panes_.SetDragScrollOffset(info.thumb_height * 0.5f);
        const float new_thumb_y = dip_y - panes_.GetDragScrollOffset();
        scroll.scroll_y = ScrollFromThumbY(info, new_thumb_y);
        scroll.max_scroll = info.max_scroll;
        cache_dirty = true;
        Invalidate();
    }
}

void App::HandleScrollbarDrag(float dip_y, const PaneScrollInfo& info,
    ScrollState& scroll, bool& cache_dirty)
{
    const float new_thumb_y = dip_y - panes_.GetDragScrollOffset();
    scroll.scroll_y = ScrollFromThumbY(info, new_thumb_y);
    scroll.max_scroll = info.max_scroll;
    cache_dirty = true;
    Invalidate();
}

// ============================================================
// カスタムタイトルバー
// ============================================================

void App::OnActivate(bool active)
{
    if (window_active_ != active) {
        window_active_ = active;
        InvalidateTitleBar();
    }
    if (!active) {
        ClearTooltip();
    }
}

// ============================================================
// ペインレイアウト
// ============================================================

const PaneLayout& App::GetPaneLayout() const
{
    if (!pane_layout_valid_) {
        auto* rt = renderer_.GetRenderTarget();
        if (!rt) {
            static const PaneLayout empty{};
            return empty;
        }
        const auto size = rt->GetSize();
        cached_window_width_for_layout_ = size.width;
        const float tb_h = titlebar_.GetHeight();
        cached_pane_layout_ = panes_.ComputeLayout(size.width, size.height,
            renderer_.GetTheme().splitter_width, tb_h);
        pane_layout_valid_ = true;
    }
    return cached_pane_layout_;
}

void App::InvalidatePane(const PaneRect& rect) noexcept
{
    const float scale = cached_dpi_scale_;
    RECT rc;
    rc.left = static_cast<LONG>(rect.x * scale);
    rc.top = static_cast<LONG>(rect.y * scale);
    rc.right = static_cast<LONG>((rect.x + rect.width) * scale) + 1;
    rc.bottom = static_cast<LONG>((rect.y + rect.height) * scale) + 1;
    InvalidateRect(hwnd_, &rc, FALSE);
}

void App::InvalidateTitleBar() noexcept
{
    const float tb_h = titlebar_.GetHeight();
    if (tb_h <= 0.0f) {
        return;
    }
    // 幅が未計算（初期化直後など）の場合はウィンドウ全体を無効化する
    if (cached_window_width_for_layout_ <= 0.0f) {
        InvalidateRect(hwnd_, nullptr, FALSE);
        return;
    }
    InvalidatePane(PaneRect{ 0.0f, 0.0f, cached_window_width_for_layout_, tb_h });
}

PaneZone App::PaneAtPoint(float dip_x, [[maybe_unused]] float dip_y) const
{
    const auto* rt = renderer_.GetRenderTarget();
    if (!rt) {
        return PaneZone::None;
    }
    const auto size = rt->GetSize();
    return panes_.DetectZone(dip_x, size.width, size.height,
        renderer_.GetTheme().splitter_width);
}

float App::GetMarkdownPaneWidth() const
{
    const auto layout = GetPaneLayout();
    return layout.md_rect.width;
}

// ============================================================
// OnPaint用のレンダーステート構築ヘルパー
// ============================================================

GestureRenderState App::BuildGestureRenderState() const
{
    GestureRenderState gs;
    gs.trail_active = gesture_.IsGestureActive();
    gs.trail_points = &gesture_.GetTrailPoints();
    gs.overlay_visible = gesture_.IsOverlayVisible();
    gs.direction = (gesture_.GetDirection() == GestureDirection::Left) ? -1
        : (gesture_.GetDirection() == GestureDirection::Right) ? 1 : 0;
    gs.overlay_alpha = gesture_.GetOverlayAlpha();

    // タッチパッドスワイプのオーバーレイ（マウスジェスチャーが非アクティブの場合のみ）
    if (!gs.overlay_visible && swipe_detector_.IsOverlayVisible()) {
        gs.overlay_visible = true;
        gs.direction = swipe_detector_.GetOverlayDirection();
        gs.overlay_alpha = swipe_detector_.GetOverlayAlpha();
    }
    return gs;
}

SidePaneState App::BuildSidePaneState(const ::PaneLayout& layout) const
{
    return { layout.file_rect, layout.toc_rect,
             file_explorer_.GetEntries(), panes_.FileScroll(), panes_.GetHoveredFileIndex(),
             doc_.GetToc().GetEntries(), panes_.TocScroll(), panes_.GetHoveredTocIndex(),
             panes_.IsFilePaneVisible(), panes_.IsTocPaneVisible(),
             panes_.IsFileCloseHovered(), panes_.IsFileRefreshHovered(),
             panes_.IsTocCloseHovered(), active_toc_index_ };
}

TitleBarRenderState App::BuildTitleBarRenderState(float window_width) const
{
    TitleBarRenderState tb;
    tb.height = titlebar_.GetHeight();
    tb.window_width = window_width;
    tb.help_btn_rect = titlebar_.GetHelpButton().rect;
    tb.help_btn_hovered = titlebar_.GetHelpButton().hovered;
    tb.theme_btn_rect = titlebar_.GetThemeToggleButton().rect;
    tb.theme_btn_hovered = titlebar_.GetThemeToggleButton().hovered;
    tb.is_dark_mode = theme_service_.IsDarkMode();
    tb.search_btn_rect = titlebar_.GetSearchButton().rect;
    tb.search_btn_hovered = titlebar_.GetSearchButton().hovered;
    tb.search_active = search_state_.IsVisible();
    tb.file_btn_rect = titlebar_.GetFileToggleButton().rect;
    tb.file_btn_hovered = titlebar_.GetFileToggleButton().hovered;
    tb.file_pane_visible = panes_.IsFilePaneVisible();
    tb.toc_btn_rect = titlebar_.GetTocToggleButton().rect;
    tb.toc_btn_hovered = titlebar_.GetTocToggleButton().hovered;
    tb.toc_pane_visible = panes_.IsTocPaneVisible();
    tb.minimize_btn_rect = titlebar_.GetMinimizeButton().rect;
    tb.minimize_btn_hovered = titlebar_.GetMinimizeButton().hovered;
    tb.maximize_btn_rect = titlebar_.GetMaximizeButton().rect;
    tb.maximize_btn_hovered = titlebar_.GetMaximizeButton().hovered;
    tb.is_maximized = IsZoomed(hwnd_) != FALSE;
    tb.close_btn_rect = titlebar_.GetCloseButton().rect;
    tb.close_btn_hovered = titlebar_.GetCloseButton().hovered;
    tb.icon_rect = titlebar_.GetIconRect();
    tb.title_text_rect = titlebar_.GetTitleTextRect();
    tb.title_text = cached_title_text_;
    tb.window_active = window_active_;
    return tb;
}

ToastRenderState App::BuildToastRenderState() const
{
    ToastRenderState ts;
    ts.visible = toast_.IsVisible();
    ts.alpha = toast_.GetRenderAlpha();
    ts.message = toast_.GetMessage();
    return ts;
}

SearchBarRenderState App::BuildSearchBarRenderState() const
{
    SearchBarRenderState sb;
    sb.visible = search_state_.IsVisible();
    sb.query = search_state_.GetQuery();
    sb.current_match = search_state_.GetCurrentMatchIndex();
    sb.total_matches = search_state_.GetMatchCount();
    sb.has_focus = search_has_focus_;
    sb.caret_visible = search_has_focus_ && search_caret_visible_;
    sb.caret_pos = search_caret_pos_;
    sb.selection_start = search_selection_start_;
    sb.ime_composition = ime_composition_;
    sb.case_sensitive = search_state_.IsCaseSensitive();
    sb.highlight_enabled = search_state_.IsHighlightEnabled();
    sb.up_btn_hovered = (search_bar_hover_ == SearchBarHover::Up);
    sb.down_btn_hovered = (search_bar_hover_ == SearchBarHover::Down);
    sb.close_btn_hovered = (search_bar_hover_ == SearchBarHover::Close);
    sb.case_btn_hovered = (search_bar_hover_ == SearchBarHover::CaseSensitive);
    sb.highlight_btn_hovered = (search_bar_hover_ == SearchBarHover::Highlight);
    return sb;
}

// ============================================================
// 描画 / リサイズ
// ============================================================

void App::OnPaint()
{
    MENDO_PROFILE("OnPaint");

    PAINTSTRUCT ps;
    BeginPaint(hwnd_, &ps);

    const auto& layout = GetPaneLayout();
    if (!file_load_service_.IsLoading()) {
        // 現在表示中のダーティなノードを現在の幅でレイアウトする
        const auto anchor = SaveAnchor();

        bool updated;
        {
            MENDO_PROFILE("EnsureVisibleLayout");
            updated = layout_service_->EnsureVisibleLayout(
                doc_, layout_cache_, layout.md_rect.width, layout.md_rect.height);
        }

        if (updated) {
            RestoreAnchor(anchor, layout.md_rect.height);
        }
    }
    // 目次ペインの同期: mdペインのスクロール位置からアクティブ見出しを判定し、
    // 目次ペインを自動スクロールする
    if (panes_.IsTocPaneVisible() && !file_load_service_.IsLoading()) {
        const float toc_margin = layout.md_rect.y + renderer_.GetTheme().heading_spacing_above;
        const int new_active = doc_.GetToc().FindActiveIndex(layout_cache_, viewport_.GetScrollY(), toc_margin);
        if (new_active != active_toc_index_) {
            active_toc_index_ = new_active;
            renderer_.InvalidateTocPaneCache();

            // アクティブ見出しが目次ペインの表示範囲外なら自動スクロール
            if (new_active >= 0) {
                const auto& theme = renderer_.GetTheme();
                const float item_y = static_cast<float>(new_active) * theme.pane_item_height;
                const float total = static_cast<float>(doc_.GetToc().GetEntries().size()) * theme.pane_item_height;
                const auto info = ComputeScrollInfo(layout.toc_rect, theme.pane_header_height, total);
                float& sy = panes_.TocScroll().scroll_y;
                if (item_y < sy || item_y + theme.pane_item_height > sy + info.content_height) {
                    sy = std::clamp(item_y - info.content_height * 0.5f, 0.0f, info.max_scroll);
                    panes_.TocScroll().max_scroll = info.max_scroll;
                }
            }
        }
    }

    const auto gs = BuildGestureRenderState();
    const auto sp = BuildSidePaneState(layout);
    const auto tb = BuildTitleBarRenderState(cached_window_width_for_layout_);
    const auto ts = BuildToastRenderState();
    const auto sb = BuildSearchBarRenderState();

    if (file_load_service_.IsLoading()) {
        renderer_.DrawLoading(file_load_service_.GetLoadingAngle(), layout.md_rect, sp, tb, gs, ts);
    }
    else {
        // 検索マッチ情報をコマンドジェネレータに設定
        if (search_state_.IsVisible() && search_state_.IsHighlightEnabled() && !search_state_.GetMatches().empty()) {
            renderer_.SetSearchMatches(&search_state_.GetMatches(), search_state_.GetCurrentMatchIndex());
        }
        else {
            renderer_.SetSearchMatches(nullptr, -1);
        }

        {
            MENDO_PROFILE("Renderer::Render");
            renderer_.Render({
                doc_.GetNodesMut(), layout_cache_,
                viewport_.GetScrollY(), layout_service_->GetTotalHeight(),
                viewport_.GetSelection(), layout.md_rect, sp, tb,
                nav_service_.CanGoBack(), nav_service_.CanGoForward(),
                static_cast<int>(nav_hover_), hovered_copy_node_, gs, ts, sb,
                layout_service_->HasDirtyNodes()
                });
        }
    }

    EndPaint(hwnd_, &ps);
}

void App::OnResize(UINT width, UINT height)
{
    if (width == 0 || height == 0) {
        return;
    }

    InvalidatePaneLayoutCache();
    renderer_.Resize(width, height);

    // タイトルバーボタン位置を再計算
    {
        const float window_w_dip = width / cached_dpi_scale_;
        titlebar_.UpdateLayout(window_w_dip);
    }

    if (is_sizing_) {
        const auto sizing_layout = GetPaneLayout();
        const float sizing_h = sizing_layout.md_rect.height;
        SyncMaxScroll(sizing_h);
        UpdateScrollBar();
        Invalidate();
        return;
    }

    OnResizeEnd();
}

void App::OnDpiChanged(UINT dpi, const RECT* suggested)
{
    cached_dpi_scale_ = static_cast<float>(dpi) / 96.0f;
    if (cached_dpi_scale_ <= 0.0f) {
        cached_dpi_scale_ = 1.0f;
    }
    renderer_.SetDpi(static_cast<float>(dpi));

    InvalidatePaneLayoutCache();
    layout_cache_.MarkAllDirty();
    file_cache_.ClearAll();

    SetWindowPos(hwnd_, nullptr,
        suggested->left, suggested->top,
        suggested->right - suggested->left,
        suggested->bottom - suggested->top,
        SWP_NOZORDER | SWP_NOACTIVATE);
}

// ============================================================
// サイズ変更状態
// ============================================================

void App::OnEnterSizeMove()
{
    is_sizing_ = true;
}

void App::OnExitSizeMove()
{
    is_sizing_ = false;
    OnResizeEnd();
}

// ============================================================
// ファイル読み込み
// ============================================================

void App::LoadHelpDocument()
{
    if (IsHelpPath(doc_.GetFilePath())) {
        return;
    }

    const auto rc = LoadRcData(i18n::S().help_resource_id);
    if (rc.empty()) {
        return;
    }

    KillTimer(hwnd_, TIMER_LOADING_ANIM);
    file_load_service_.StopLoading();
    viewport_.ClearSelection();
    CancelMermaidBatch();
    image_loader_.CancelPending();
    renderer_.ShrinkBuffers();
    resolved_image_paths_.clear();
    doc_service_.StopWatching();

    std::pmr::string utf8(reinterpret_cast<const char*>(rc.data()), rc.size());
    doc_ = Document::FromMarkdown(std::move(utf8), HELP_PATH);
    layout_cache_.Reset(doc_.GetNodes().size());

    file_explorer_.SetCurrentFile(L"");
    renderer_.InvalidateFilePaneCache();

    panes_.ResetScrollStates();
    renderer_.InvalidateTocPaneCache();

    // ビューポート優先レイアウト + 遅延処理
    viewport_.SetScrollY(0.0f);
    {
        const auto pane_layout = GetPaneLayout();
        layout_service_->ViewportLayout(doc_, layout_cache_,
            pane_layout.md_rect.width, pane_layout.md_rect.height);
        SyncMaxScroll(pane_layout.md_rect.height);
    }
    UpdateScrollBar();
    Invalidate();
    ScheduleDeferredLayoutIfNeeded();

    UpdateTitleBar();
}

void App::LoadMarkdownFile(std::wstring_view path)
{
    const std::pmr::wstring path_str{ path };
    if (!DocumentService::NeedsLoadingAnimation(path_str)) {
        file_load_service_.SetLoadingPath(path_str);
        DoLoadMarkdownFile();
    }
    else {
        file_load_service_.StartLoading(path_str);
        SetTimer(hwnd_, TIMER_LOADING_ANIM, 16, nullptr);
        Invalidate();
        UpdateWindow(hwnd_);
        PostMessage(hwnd_, WM_APP_LOAD_FILE, 0, 0);
    }
}

void App::DoLoadMarkdownFile()
{
    MENDO_PROFILE("DoLoadMarkdownFile");

    // ヘルプ仮想パスの場合は専用ルートへ
    if (IsHelpPath(file_load_service_.GetLoadingPath())) {
        LoadHelpDocument();
        return;
    }

    KillTimer(hwnd_, TIMER_LOADING_ANIM);

    viewport_.ClearSelection();
    search_state_.Reset();
    // ファイル切替時はEDITコントロールのテキストもクリア
    PostMessage(hwnd_, WM_APP_SEARCH_UNFOCUS, SEARCH_UNFOCUS_FILE_SWITCH, 0);
    active_toc_index_ = -1;
    CancelMermaidBatch();
    image_loader_.CancelPending();
    renderer_.ShrinkBuffers();
    resolved_image_paths_.clear();

    {
        MENDO_PROFILE("ExecuteLoad(FileIO+Parse)");
        if (!file_load_service_.ExecuteLoad(doc_, layout_cache_)) {
            Invalidate();
            return;
        }
    }

    const std::pmr::wstring dir = doc_.GetDirectory();
    if (!dir.empty()) {
        file_explorer_.SetDirectory(dir);
        file_explorer_.SetCurrentFile(doc_.GetFilePath());
    }

    panes_.ResetScrollStates();
    renderer_.InvalidateFilePaneCache();
    renderer_.InvalidateTocPaneCache();

    // ビューポート優先レイアウト: 可視範囲のみ計測し、残りは遅延処理に委ねる。
    const auto pane_layout = GetPaneLayout();
    const float md_width = pane_layout.md_rect.width;
    const float md_height = pane_layout.md_rect.height;

    // スクロール位置の復元
    float scroll_y = 0.0f;
    if (pending_restore_node_ >= 0) {
        // cache.Reset()直後はY座標が全て0のため、DirectWriteを使わない
        // 軽量パスでノード高さとY座標を推定する（O(n)の算術演算のみ）。
        // 遅延レイアウトのアンカー補償により正確な位置へ収束する。
        {
            MENDO_PROFILE("EstimateNodeHeights");
            EstimateNodeHeights(doc_.GetNodes(), layout_cache_, renderer_.GetTheme());
        }
        // Mermaidブロックの推定高さをファイルキャッシュの実測値で上書きする。
        // 行数ベースの推定は実際の描画サイズと大きく乖離し得るため、
        // キャッシュ済みの正確な高さを使うことでスクロール復元時のジャンプを防ぐ。
        {
            const float content_width = renderer_.GetTheme().ContentWidth(md_width);
            const bool dark_mode = theme_service_.IsDarkMode();
            const auto& nodes = doc_.GetNodes();
            for (size_t i : doc_.GetMermaidNodeIndices()) {
                const auto hash = mermaid_util::HashCode(
                    nodes[i].text, content_width, dark_mode);
                MermaidFileCache::CacheEntry fentry;
                if (file_cache_.LookupDimensions(hash, fentry)) {
                    layout_cache_[i].height = fentry.css_height;
                }
            }
            RecomputeYPositions(doc_.GetNodesMut(), layout_cache_, renderer_.GetTheme());
        }
        const int node = std::min(pending_restore_node_,
            static_cast<int>(layout_cache_.size()) - 1);
        if (node >= 0) {
            scroll_y = std::max(0.0f,
                layout_cache_[node].y_position + static_cast<float>(pending_restore_offset_));
        }
        pending_restore_node_ = -1;
    }
    else if (pending_nav_scroll_y_ >= 0.0f) {
        scroll_y = pending_nav_scroll_y_;
        pending_nav_scroll_y_ = -1.0f;
    }

    // スクロール位置を設定してからViewportLayoutを呼ぶことで、
    // 復元先の可視範囲のノードが計測される
    viewport_.SetScrollY(scroll_y);

    {
        MENDO_PROFILE("ViewportLayout(Initial)");
        layout_service_->ViewportLayout(doc_, layout_cache_, md_width, md_height);
    }

    // キャッシュ済みリソースを適用（画像/Mermaidの正確な高さを反映）
    {
        MENDO_PROFILE("LoadImages");
        LoadImages();
    }
    {
        MENDO_PROFILE("RequestMermaidRenders(visible)");
        RequestMermaidRenders();
    }

    // キャッシュ適用後にスクロール範囲を確定
    SyncMaxScroll(md_height);
    UpdateScrollBar();
    Invalidate();

    // 残りのダーティノードを遅延レイアウトで処理
    ScheduleDeferredLayoutIfNeeded();

    UpdateTitleBar();

    doc_service_.StartWatching(doc_.GetFilePath(), [this]() {
        ReloadCurrentFile();
    });
}

void App::ReloadCurrentFile()
{
    if (doc_.GetFilePath().empty() || IsHelpPath(doc_.GetFilePath())) {
        return;
    }
    // ローディングアニメーション表示中は重複リロードを抑制
    if (file_load_service_.IsLoading()) {
        return;
    }

    if (DocumentService::NeedsLoadingAnimation(doc_.GetFilePath())) {
        file_load_service_.StartLoading(doc_.GetFilePath());
        SetTimer(hwnd_, TIMER_LOADING_ANIM, 16, nullptr);
        Invalidate();
        UpdateWindow(hwnd_);
        PostMessage(hwnd_, WM_APP_RELOAD_FILE, 0, 0);
    }
    else {
        DoReloadCurrentFile();
    }
}

void App::DoReloadCurrentFile()
{
    MENDO_PROFILE("DoReloadCurrentFile");

    KillTimer(hwnd_, TIMER_LOADING_ANIM);
    file_load_service_.StopLoading();
    active_toc_index_ = -1;

    if (doc_.GetFilePath().empty()) {
        return;
    }

    const float old_scroll = viewport_.GetScrollY();

    CancelMermaidBatch();
    image_loader_.CancelPending();
    resolved_image_paths_.clear();

    // ファイルを読み込み、旧コンテンツとの差分位置をコピーなしで計算
    std::pmr::string new_utf8;
    {
        MENDO_PROFILE("Reload::LoadFile");
        new_utf8 = FileLoader::LoadFile(doc_.GetFilePath());
    }
    const size_t diff_pos = FindFirstDifference(
        std::string_view(doc_.GetRawUtf8()),
        std::string_view(new_utf8));

    // ドキュメントを新コンテンツで更新
    {
        MENDO_PROFILE("Reload::ReplaceFromMarkdown");
        doc_.ReplaceFromMarkdown(std::move(new_utf8));
    }
    layout_cache_.Reset(doc_.GetNodes().size(), false);

    renderer_.InvalidateTocPaneCache();

    const auto pane_layout = GetPaneLayout();
    const float md_width = pane_layout.md_rect.width;
    const float md_height = pane_layout.md_rect.height;

    // Reset直後はheight/y_positionが全て0のため、軽量推定でY座標を確定させる
    EstimateNodeHeights(doc_.GetNodes(), layout_cache_, renderer_.GetTheme());

    // 変更箇所のスクロール位置を決定（推定高さベース）
    float desired_scroll = old_scroll;
    const auto& new_content = doc_.GetRawUtf8();
    const auto& nodes = doc_.GetNodes();

    if (diff_pos != std::string_view::npos && !nodes.empty()) {
        const int changed_node = FindNodeBySourceOffset(nodes, static_cast<uint32_t>(diff_pos));
        if (changed_node >= 0 && changed_node < static_cast<int>(layout_cache_.size())) {
            float node_y = layout_cache_[changed_node].y_position;
            const float node_h = layout_cache_[changed_node].height;

            // ノード内での相対位置を推定してY座標を補正
            const uint32_t node_start = nodes[changed_node].source_offset;
            if (node_start != UINT32_MAX) {
                uint32_t next_start = static_cast<uint32_t>(new_content.size());
                for (int i = changed_node + 1; i < static_cast<int>(nodes.size()); ++i) {
                    if (nodes[i].source_offset != UINT32_MAX && nodes[i].source_offset > node_start) {
                        next_start = nodes[i].source_offset;
                        break;
                    }
                }
                if (next_start > node_start) {
                    const float fraction = static_cast<float>(diff_pos - node_start)
                        / static_cast<float>(next_start - node_start);
                    node_y += node_h * std::min(fraction, 1.0f);
                }
            }

            const float margin = md_height * 0.2f;
            desired_scroll = std::max(0.0f, node_y - margin);
        }
    }

    // スクロール位置を設定してからViewportLayoutを呼ぶことで、
    // 変更箇所周辺の可視ノードが計測される
    viewport_.SetScrollY(desired_scroll);

    {
        MENDO_PROFILE("Reload::ViewportLayout");
        layout_service_->ViewportLayout(doc_, layout_cache_, md_width, md_height);
    }

    // キャッシュ済み画像/Mermaidを適用してY位置を正確にする
    LoadImages();
    RequestMermaidRenders();

    SyncMaxScroll(md_height);
    UpdateScrollBar();

    // 検索バー表示中なら再検索
    if (search_state_.IsVisible() && !search_state_.GetQuery().empty()) {
        RunSearchAndLocate();
    }

    Invalidate();

    // 残りのダーティノードを遅延レイアウトで処理
    ScheduleDeferredLayoutIfNeeded();

    // OSが同一セーブ操作で複数イベントを送るため、処理完了時点から
    // デバウンスを再計測しないと重複リロードが発生する
    doc_service_.ResetDebounceTick();
}

void App::UpdateTitleBar()
{
    const int zoom_percent = static_cast<int>(ZOOM_STEPS[viewport_.GetZoomIndex()] * 100.0f + 0.5f);
    auto title = BuildTitleString(doc_.GetFilePath(), zoom_percent);
    SetWindowTextW(hwnd_, title.c_str());
    cached_title_text_ = std::move(title);
    InvalidateTitleBar();
}

// キャッシュ済み画像を DiagramEntry に反映する。
// 戻り値: キャッシュから反映できた画像の数
int App::ApplyCachedImages()
{
    const std::wstring doc_dir{ doc_.GetDirectory() };
    if (doc_dir.empty()) {
        return 0;
    }

    const float viewport_width = GetMarkdownPaneWidth();
    const float content_width = renderer_.GetTheme().ContentWidth(viewport_width);
    if (content_width <= 0.0f) {
        return 0;
    }

    const float viewport_top = viewport_.GetScrollY();
    const float viewport_height = GetPaneLayout().md_rect.height;
    const float buffer = viewport_height * PREFETCH_BUFFER_SCREENS;
    const float range_top = viewport_top - buffer;
    const float range_bottom = viewport_top + viewport_height + buffer;

    int applied = 0;
    auto& nodes = doc_.GetNodesMut();
    for (size_t i : doc_.GetImageNodeIndices()) {
        auto& node = nodes[i];
        auto& diagram = layout_cache_.GetDiagram(i);
        if (diagram.bitmap) {
            continue;
        }

        if (!node.has_image() || node.image_data->src.find(L"://") != std::pmr::wstring::npos) {
            continue;
        }

        if (viewport_height > 0.0f && IsOffscreen(layout_cache_[i].y_position, layout_cache_[i].height, range_top, range_bottom)) {
            continue;
        }

        // 解決済みパスのキャッシュを確認し、ディスクI/Oを回避
        const auto cache_it = resolved_image_paths_.find(i);
        std::wstring abs_str;
        if (cache_it != resolved_image_paths_.end()) {
            abs_str = cache_it->second;
        }
        else {
            std::filesystem::path img_path(node.image_data->src);
            if (img_path.is_relative()) {
                img_path = std::filesystem::path(doc_dir) / img_path;
            }

            std::error_code ec;
            const auto abs_path = std::filesystem::canonical(img_path, ec);
            if (ec) {
                continue;
            }
            abs_str = abs_path.wstring();
            resolved_image_paths_[i] = abs_str;
        }

        if (image_loader_.GetCachedImage(abs_str, diagram)) {
            node.image_data->width = diagram.width;
            node.image_data->height = diagram.height;

            const float indent = node.indent_level * renderer_.GetTheme().indent_width;
            const float node_width = content_width - indent;
            float h = diagram.height;
            if (diagram.width > node_width && diagram.width > 0) {
                h *= node_width / diagram.width;
            }
            layout_cache_[i].height = h;
            layout_cache_[i].layout_dirty = false;
            ++applied;
        }
        else {
            image_loader_.RequestLoadAsync(abs_str,
                [](void* ctx) static { static_cast<App*>(ctx)->OnImageLoadComplete(); },
                this);
        }
    }
    return applied;
}

void App::LoadImages()
{
    // ビューポート付近の画像のみ読み込み、遠方の画像はスクロール時に遅延読み込みする
    if (ApplyCachedImages() > 0) {
        layout_service_->RecomputeAfterDiagram(doc_, layout_cache_, renderer_.GetTheme());
        Invalidate();
    }
}

void App::OnAppImageLoaded()
{
    image_loader_.ProcessCompletedDecodes();
}

void App::OnImageLoadComplete()
{
    // ビューポート付近の画像のみ反映し、遠方のノードへの再読み込み連鎖を防ぐ
    if (ApplyCachedImages() > 0) {
        const auto anchor = SaveAnchor();
        layout_service_->RecomputeAfterDiagram(doc_, layout_cache_, renderer_.GetTheme());
        const auto layout = GetPaneLayout();
        RestoreAnchor(anchor, layout.md_rect.height);
        Invalidate();
    }
}

void App::RequestMermaidRenders()
{
    const float viewport_width = GetMarkdownPaneWidth();
    const float content_width = renderer_.GetTheme().ContentWidth(viewport_width);

    // コンテンツ幅が0以下の場合（ズームでMDペインが極小になった場合など）は
    // 不正な幅でレンダリングしないようスキップする。
    // last_mermaid_content_width_ を更新しないことで、復帰時の幅変更検出を正しく保つ。
    if (content_width <= 0.0f) {
        return;
    }

    if (last_mermaid_content_width_ > 0.0f &&
        mermaid_util::QuantizeWidth(content_width) != mermaid_util::QuantizeWidth(last_mermaid_content_width_)) {
        // 図のサイズが新旧どちらのコンテンツ幅より小さければ
        // ビューポートに制約されていないため再生成不要
        const float min_width = std::min(content_width, last_mermaid_content_width_);
        bool any_invalidated = false;
        for (size_t i : doc_.GetMermaidNodeIndices()) {
            auto& diagram = layout_cache_.GetDiagram(i);
            if (diagram.bitmap && diagram.width > 0 &&
                diagram.width + 1.0f < min_width) {
                continue;
            }
            diagram.bitmap.Reset();
            diagram.width = 0;
            diagram.height = 0;
            any_invalidated = true;
        }
        if (any_invalidated) {
            mermaid_renderer_.ClearCache();
        }
        mermaid_renderer_.ClearPendingQueue();
    }
    last_mermaid_content_width_ = content_width;

    // 可視範囲のMermaidノードのみ処理する。
    // ファイルキャッシュLookup（ディスクI/O+WICデコード）が重いため、
    // 遠方のノードはスクロール時に遅延読み込みする。
    const float viewport_top = viewport_.GetScrollY();
    const float viewport_bottom = viewport_top + GetPaneLayout().md_rect.height;

    for (size_t i : doc_.GetMermaidNodeIndices()) {
        auto& node = doc_.GetNodesMut()[i];
        auto& diagram = layout_cache_.GetDiagram(i);
        if (diagram.bitmap) {
            continue;
        }

        if (IsOffscreen(layout_cache_[i].y_position, layout_cache_[i].height, viewport_top, viewport_bottom)) {
            continue;
        }

        mermaid_renderer_.RequestRender(node, layout_cache_[i], diagram,
            content_width, theme_service_.IsDarkMode(),
            [](void* ctx) static { static_cast<App*>(ctx)->OnMermaidRenderComplete(); },
            this);
    }
}

void App::OnMermaidRenderComplete()
{
    if (mermaid_batch_loading_) {
        return;
    }
    const auto anchor = SaveAnchor();
    layout_service_->RecomputeAfterDiagram(doc_, layout_cache_, renderer_.GetTheme());
    const auto layout = GetPaneLayout();
    RestoreAnchor(anchor, layout.md_rect.height);
    Invalidate();
}

void App::CancelMermaidBatch()
{
    mermaid_renderer_.CancelPending();
    KillTimer(hwnd_, TIMER_MERMAID_BATCH);
}

void App::ScheduleMermaidBatch()
{
    mermaid_batch_next_ = 0;
    SetTimer(hwnd_, TIMER_MERMAID_BATCH, 16, nullptr);
}

void App::ProcessMermaidBatch()
{
    MENDO_PROFILE("ProcessMermaidBatch");

    const float viewport_width = GetMarkdownPaneWidth();
    const float content_width = renderer_.GetTheme().ContentWidth(viewport_width);
    if (content_width <= 0.0f) {
        KillTimer(hwnd_, TIMER_MERMAID_BATCH);
        return;
    }

    const float viewport_top = viewport_.GetScrollY();
    const float viewport_height = GetPaneLayout().md_rect.height;
    const float buffer = viewport_height * EVICT_BUFFER_SCREENS;
    const float range_top = viewport_top - buffer;
    const float range_bottom = viewport_top + viewport_height + buffer;

    const bool dark_mode = theme_service_.IsDarkMode();
    const auto& indices = doc_.GetMermaidNodeIndices();
    const auto anchor = SaveAnchor();
    bool any_loaded = false;

    const auto start = std::chrono::steady_clock::now();

    // mermaid_batch_loading_ は RequestRender 内の同期キャッシュヒット時に
    // OnMermaidRenderComplete が再入呼び出しされるのを抑制する。
    mermaid_batch_loading_ = true;
    while (mermaid_batch_next_ < indices.size()) {
        const size_t i = indices[mermaid_batch_next_];

        if (viewport_height > 0.0f && IsOffscreen(layout_cache_[i].y_position, layout_cache_[i].height, range_top, range_bottom)) {
            mermaid_batch_next_++;
            continue;
        }

        auto& node = doc_.GetNodesMut()[i];
        auto& diagram = layout_cache_.GetDiagram(i);

        if (!diagram.bitmap) {
            mermaid_renderer_.RequestRender(node, layout_cache_[i], diagram,
                content_width, dark_mode,
                [](void* ctx) static { static_cast<App*>(ctx)->OnMermaidRenderComplete(); },
                this);
            if (diagram.bitmap) {
                any_loaded = true;
            }
        }

        mermaid_batch_next_++;

        const auto elapsed = std::chrono::steady_clock::now() - start;
        if (std::chrono::duration_cast<std::chrono::microseconds>(elapsed).count() >= BATCH_TIME_BUDGET_US) {
            break;
        }
    }
    mermaid_batch_loading_ = false;

    if (any_loaded) {
        layout_service_->RecomputeAfterDiagram(doc_, layout_cache_, renderer_.GetTheme());
        const auto layout = GetPaneLayout();
        RestoreAnchor(anchor, layout.md_rect.height);
        Invalidate();
    }

    if (mermaid_batch_next_ >= indices.size()) {
        KillTimer(hwnd_, TIMER_MERMAID_BATCH);
    }
}

// ============================================================
// ビューポート外リソースの解放 / 再読み込み
// ============================================================

void App::EvictOffscreenBitmaps()
{
    const float viewport_top = viewport_.GetScrollY();
    const float viewport_height = GetPaneLayout().md_rect.height;
    if (viewport_height <= 0.0f) {
        return;
    }

    const float buffer = viewport_height * EVICT_BUFFER_SCREENS;
    const float evict_top = viewport_top - buffer;
    const float evict_bottom = viewport_top + viewport_height + buffer;

    const size_t node_count = doc_.GetNodes().size();

    // テキストレイアウトの解放（最大のメモリ消費源）
    // y_position と height は保持し、layout_dirty を設定する。
    // スクロール時に EnsureVisibleLayout で再作成される。
    // 二分探索で保持範囲の前後のみ走査する。
    const int first_keep = FindFirstVisibleNodeIndex(layout_cache_, node_count, evict_top);
    int last_keep = first_keep;
    for (int i = first_keep; i < static_cast<int>(node_count); i++) {
        if (layout_cache_[i].y_position > evict_bottom) {
            break;
        }
        last_keep = i + 1;
    }

    auto evict_text_layout = [&](size_t i) {
        auto& entry = layout_cache_[i];
        if (!entry.text_layout && entry.cell_layouts.empty()) {
            return;
        }
        entry.text_layout.Reset();
        entry.effects_applied = false;
        entry.inline_code_bgs.clear();
        entry.cell_layouts.clear();
        entry.cell_inline_code_bgs.clear();
        entry.layout_dirty = true;
    };

    for (size_t i = 0; i < static_cast<size_t>(first_keep); i++) {
        evict_text_layout(i);
    }
    for (size_t i = static_cast<size_t>(last_keep); i < node_count; i++) {
        evict_text_layout(i);
    }

    // 画像: DiagramEntry と ImageLoader キャッシュの両方から解放
    for (size_t i : doc_.GetImageNodeIndices()) {
        auto& diagram = layout_cache_.GetDiagram(i);
        if (!diagram.bitmap) {
            continue;
        }
        if (IsOffscreen(layout_cache_[i].y_position, layout_cache_[i].height, evict_top, evict_bottom)) {
            diagram.bitmap.Reset();
            const auto path_it = resolved_image_paths_.find(i);
            if (path_it != resolved_image_paths_.end()) {
                image_loader_.RemoveCached(path_it->second);
            }
        }
    }

    // Mermaid: DiagramEntry を解放し、メモリキャッシュ全体をクリア
    bool any_mermaid_evicted = false;
    for (size_t i : doc_.GetMermaidNodeIndices()) {
        auto& diagram = layout_cache_.GetDiagram(i);
        if (!diagram.bitmap) {
            continue;
        }
        if (IsOffscreen(layout_cache_[i].y_position, layout_cache_[i].height, evict_top, evict_bottom)) {
            diagram.bitmap.Reset();
            any_mermaid_evicted = true;
        }
    }
    if (any_mermaid_evicted) {
        // メモリキャッシュをクリア。可視ノードの DiagramEntry は ComPtr で
        // ビットマップを保持し続けるため、表示には影響しない。
        // ファイルキャッシュがバックアップとして機能する。
        mermaid_renderer_.ClearCache();
    }
}

void App::ScheduleBitmapManage()
{
    SetTimer(hwnd_, TIMER_BITMAP_MANAGE, 150, nullptr);
}

void App::OnBitmapManageTimer()
{
    KillTimer(hwnd_, TIMER_BITMAP_MANAGE);

    EvictOffscreenBitmaps();

    if (ApplyCachedImages() > 0) {
        layout_service_->RecomputeAfterDiagram(doc_, layout_cache_, renderer_.GetTheme());
    }
    RequestMermaidRenders();     // Mermaid: 可視範囲のみ（メモリ/ファイルキャッシュ経由）

    Invalidate();
}

// ============================================================
// マウスホイール / キーボード
// ============================================================

void App::OnMouseWheel(int px, int py, short delta, bool ctrl)
{
    if (!renderer_.GetRenderTarget()) {
        return;
    }

    // 軸ロック用: 縦スクロール発生をスワイプ検出器に通知
    if (!ctrl) {
        const bool had_overlay = swipe_detector_.IsOverlayVisible();
        swipe_detector_.NotifyVScroll(GetTickCount64());
        if (had_overlay) {
            KillTimer(hwnd_, TIMER_SWIPE_OVERLAY);
            Invalidate();
        }
    }

    if (ctrl) {
        const MouseWheelEvent event{ delta, true, PaneZone::MdPane };
        ExecuteActions(controller_.HandleMouseWheel(event));
        return;
    }

    const auto dip = PixelToDip(px, py);
    const auto pane_layout = GetPaneLayout();
    const auto zone = DetectPaneZone(dip.x, pane_layout,
        renderer_.GetTheme().splitter_width,
        panes_.IsFilePaneVisible(), panes_.IsTocPaneVisible());

    const MouseWheelEvent event{ delta, false, zone };
    ExecuteActions(controller_.HandleMouseWheel(event));
}

void App::OnMouseHWheel(short delta)
{
    const bool had_overlay = swipe_detector_.IsOverlayVisible();
    const int old_direction = swipe_detector_.GetOverlayDirection();
    swipe_detector_.OnHWheel(delta, GetTickCount64());

    // 入力のたびにコミットタイマーをリセット。
    // 指を離して COMMIT_TIMEOUT_MS 経過後に Commit() でナビゲーション判定する。
    SetTimer(hwnd_, TIMER_SWIPE_OVERLAY,
        static_cast<UINT>(SwipeDetector::COMMIT_TIMEOUT_MS), nullptr);

    if (had_overlay != swipe_detector_.IsOverlayVisible()
        || old_direction != swipe_detector_.GetOverlayDirection()) {
        Invalidate();
    }
}

void App::OnKeyDown(WPARAM key)
{
    const KeyDownEvent event{
        static_cast<int>(key),
        (GetKeyState(VK_CONTROL) & 0x8000) != 0,
        (GetKeyState(VK_SHIFT) & 0x8000) != 0,
        (GetKeyState(VK_MENU) & 0x8000) != 0
    };
    ExecuteActions(controller_.HandleKeyDown(event));
}

void App::ExecuteActions(const ActionList& actions)
{
    for (const auto& action : actions) {
        std::visit(overloaded{
            [this](const KeyScrollAction& a) {
                pending_restore_scroll_y_ = -1;
                const float old_scroll = viewport_.GetScrollY();
                const auto pane_layout = GetPaneLayout();
                const float page_size = pane_layout.md_rect.height;
                switch (a.type) {
                    case ScrollType::LineUp:   viewport_.DirectScrollBy(-40.0f); break;
                    case ScrollType::LineDown: viewport_.DirectScrollBy(40.0f); break;
                    case ScrollType::PageUp:   viewport_.DirectScrollBy(-page_size * 0.9f); break;
                    case ScrollType::PageDown: viewport_.DirectScrollBy(page_size * 0.9f); break;
                    case ScrollType::Home:     viewport_.ScrollTo(0.0f); break;
                    case ScrollType::End:      viewport_.ScrollTo(viewport_.GetMaxScroll()); break;
                }
                if (viewport_.GetScrollY() != old_scroll) {
                    InvalidateHitPositions();
                    Invalidate();
                    ScheduleBitmapManage();
                }
            },
            [this](const DirectScrollByAction& a) {
                pending_restore_scroll_y_ = -1;
                viewport_.DirectScrollBy(a.delta);
                InvalidateHitPositions();
                Invalidate();
                ScheduleBitmapManage();
            },
            [this](const ScrollPaneAction& a) {
                const auto pane_layout = GetPaneLayout();
                const auto& theme = renderer_.GetTheme();
                if (a.pane == PaneZone::FilePane) {
                    const float max_file_scroll = std::max(0.0f,
                        static_cast<float>(file_explorer_.GetEntries().size()) * theme.pane_item_height
                        - (pane_layout.file_rect.height - theme.pane_header_height));
                    if (panes_.ScrollFilePaneBy(a.delta, max_file_scroll)) {
                        renderer_.InvalidateFilePaneCache();
                        Invalidate();
                    }
                }
                else if (a.pane == PaneZone::TocPane) {
                    const float max_toc_scroll = std::max(0.0f, static_cast<float>(doc_.GetToc().GetEntries().size()) * theme.pane_item_height - (pane_layout.toc_rect.height - theme.pane_header_height));
                    if (panes_.ScrollTocPaneBy(a.delta, max_toc_scroll)) {
                        renderer_.InvalidateTocPaneCache();
                        Invalidate();
                    }
                }
            },
            [this](const CopyClipboardAction&) {
                CopySelectionToClipboard();
            },
            [this](const SelectAllAction&) {
                SelectAll();
            },
            [this](const ClearSelectionAction&) {
                if (search_state_.IsVisible()) {
                    OnSearchClose();
                }
                else {
                    ClearSelection();
                }
            },
            [this](const TogglePaneAction& a) {
                if (a.file_pane) {
                    panes_.ToggleFilePane();
                }
                else {
                    panes_.ToggleTocPane();
                }
                RefreshPaneLayout();
            },
            [this](const ZoomAction& a) {
                if (a.direction > 0) {
                    ZoomIn();
                }
                else if (a.direction < 0) {
                    ZoomOut();
                }
                else {
                    ZoomReset();
                }
            },
            [this](const ReloadFileAction&) {
                ReloadCurrentFile();
            },
            [this](const OpenFileAction&) {
                const auto path = FileLoader::OpenFileDialog(hwnd_);
                if (!path.empty()) {
                    if (!doc_.GetFilePath().empty()) {
                        PushNavHistory();
                    }
                    LoadMarkdownFile(path);
                }
            },
            [this](const ToggleDarkModeAction&) {
                ToggleDarkMode();
            },
            [this](const NavigateBackAction&) {
                NavigateBack();
            },
            [this](const NavigateForwardAction&) {
                NavigateForward();
            },
            [this](const ShowHelpAction&) {
                if (!doc_.GetFilePath().empty() && !IsHelpPath(doc_.GetFilePath())) {
                    PushNavHistory();
                }
                LoadHelpDocument();
            },
            [this](const OpenSearchBarAction&) {
                OnSearchOpen();
            },
            [this](const CloseSearchBarAction&) {
                OnSearchClose();
            },
            [this](const SearchNextAction&) {
                if (!search_state_.IsVisible()) {
                    OnSearchOpen();
                }
                else {
                    OnSearchNext();
                }
            },
            [this](const SearchPrevAction&) {
                if (!search_state_.IsVisible()) {
                    OnSearchOpen();
                }
                else {
                    OnSearchPrev();
                }
            },
            }, action);
    }
}

void App::OnDropFiles(HDROP hDrop)
{
    const UINT required = DragQueryFileW(hDrop, 0, nullptr, 0);
    if (required > 0) {
        std::pmr::wstring path(required, L'\0');
        if (DragQueryFileW(hDrop, 0, path.data(), required + 1)) {
            if (!doc_.GetFilePath().empty()) {
                PushNavHistory();
            }
            LoadMarkdownFile(path);
        }
    }
    DragFinish(hDrop);
}

void App::HandleTimer(UINT_PTR timer_id)
{
    MENDO_PROFILE("HandleTimer");
    switch (timer_id) {
    case TIMER_FILE_WATCH:     doc_service_.CheckForChanges(); break;
    case TIMER_DEFERRED_LAYOUT: OnDeferredLayout(); break;
    case TIMER_LOADING_ANIM:
        file_load_service_.TickLoadingAnimation();
        Invalidate();
        break;
    case TIMER_SWIPE_OVERLAY: {
        const auto result = swipe_detector_.Commit();
        bool need_redraw = false;
        switch (result) {
        case SwipeResult::Back:
            NavigateBack();
            need_redraw = true;
            break;
        case SwipeResult::Forward:
            NavigateForward();
            need_redraw = true;
            break;
        default:
            break;
        }
        KillTimer(hwnd_, TIMER_SWIPE_OVERLAY);
        if (need_redraw) {
            Invalidate();
        }
        break;
    }
    case TIMER_TOAST: {
        if (!toast_.Tick()) {
            KillTimer(hwnd_, TIMER_TOAST);
        }
        Invalidate();
        break;
    }
    case TIMER_SEARCH_CARET: {
        search_caret_visible_ = !search_caret_visible_;
        if (search_has_focus_) {
            InvalidateSearchBar();
        }
        break;
    }
    case TIMER_TOOLTIP:
        KillTimer(hwnd_, TIMER_TOOLTIP);
        tooltip_.Show();
        break;
    case TIMER_SEARCH_DEBOUNCE:
        KillTimer(hwnd_, TIMER_SEARCH_DEBOUNCE);
        RunSearchAndLocate(true);
        Invalidate();
        break;
    case TIMER_MERMAID_BATCH:
        ProcessMermaidBatch();
        break;
    case TIMER_BITMAP_MANAGE:
        OnBitmapManageTimer();
        break;
    default: break;
    }
}

void App::OnAppLoadFile()
{
    DoLoadMarkdownFile();
}

void App::OnAppReloadFile()
{
    DoReloadCurrentFile();
}

void App::OnCaptureChanged()
{
    search_dragging_ = false;
    if (gesture_.GetPhase() != GesturePhase::Idle) {
        gesture_.Reset();
        Invalidate();
    }
}

void App::ShowToast(std::wstring_view message)
{
    toast_.Show(message);
    SetTimer(hwnd_, TIMER_TOAST, 16, nullptr);
    Invalidate();
}

void App::OnDestroy()
{
    scheduler_.Shutdown();
    file_cache_.SaveIndex();
    SaveLastFilePath();
    SavePaneState();
    SaveScrollPosition();
    config_.SaveWString("General", "Language", i18n::GetLangKey());
    KillTimer(hwnd_, TIMER_FILE_WATCH);
    KillTimer(hwnd_, TIMER_DEFERRED_LAYOUT);
    KillTimer(hwnd_, TIMER_LOADING_ANIM);
    KillTimer(hwnd_, TIMER_SWIPE_OVERLAY);
    KillTimer(hwnd_, TIMER_TOAST);
    KillTimer(hwnd_, TIMER_SEARCH_CARET);
    KillTimer(hwnd_, TIMER_SEARCH_DEBOUNCE);
    KillTimer(hwnd_, TIMER_TOOLTIP);
    KillTimer(hwnd_, TIMER_BITMAP_MANAGE);
    KillTimer(hwnd_, TIMER_MERMAID_BATCH);
}

// ============================================================
// 検索
// ============================================================

void App::RunSearchAndLocate(bool scroll_to_match)
{
    search_state_.ExecuteSearch(doc_.GetNodes());
    if (search_state_.GetMatchCount() > 0) {
        search_state_.SetCurrentMatchNear(viewport_.GetScrollY(), layout_cache_);
        if (scroll_to_match) {
            ScrollToCurrentMatch();
        }
    }
}

void App::OnSearchOpen()
{
    if (search_state_.IsVisible()) {
        // 既に表示中ならトグルで閉じる
        OnSearchClose();
        return;
    }
    search_state_.Show();
    search_has_focus_ = true;
    search_caret_visible_ = true;
    search_caret_pos_ = -1;
    search_selection_start_ = -1;

    // 前回のクエリが残っている場合は検索を再実行
    if (!search_state_.GetQuery().empty()) {
        RunSearchAndLocate();
    }

    RestartSearchCaretBlink();
    PostMessage(hwnd_, WM_APP_SEARCH_FOCUS, SEARCH_FOCUS_SELECT_ALL, 0);
    Invalidate();
}

void App::OnSearchClose()
{
    search_state_.Hide();
    search_bar_hover_ = SearchBarHover::None;
    search_has_focus_ = false;
    search_caret_visible_ = false;
    ime_composition_.clear();
    KillTimer(hwnd_, TIMER_SEARCH_CARET);
    KillTimer(hwnd_, TIMER_SEARCH_DEBOUNCE);
    PostMessage(hwnd_, WM_APP_SEARCH_UNFOCUS, 0, 0);
    Invalidate();
}

void App::OnSearchNext()
{
    if (search_state_.NextMatch() && search_state_.GetMatchCount() > 1) {
        MessageBeep(MB_OK);
    }
    ScrollToCurrentMatch();
    Invalidate();
}

void App::OnSearchPrev()
{
    if (search_state_.PrevMatch() && search_state_.GetMatchCount() > 1) {
        MessageBeep(MB_OK);
    }
    ScrollToCurrentMatch();
    Invalidate();
}

void App::OnSearchTextChanged(std::wstring_view text)
{
    search_state_.SetQuery(text);
    KillTimer(hwnd_, TIMER_SEARCH_DEBOUNCE);

    if (text.empty()) {
        // 空クエリ: 即座に結果をクリア
        search_state_.ExecuteSearch(doc_.GetNodes());
        Invalidate();
        return;
    }

    // 小規模ドキュメント（≤1000ノード）: 即座に検索実行
    if (doc_.GetNodes().size() <= 1000) {
        RunSearchAndLocate(true);
        Invalidate();
        return;
    }

    // 大規模ドキュメント: デバウンスで連続入力中の再検索を抑制
    Invalidate();
    SetTimer(hwnd_, TIMER_SEARCH_DEBOUNCE, 150, nullptr);
}

void App::OnToggleCaseSensitive()
{
    search_state_.ToggleCaseSensitive();
    if (!search_state_.GetQuery().empty()) {
        RunSearchAndLocate();
    }
    Invalidate();
}

void App::OnToggleHighlight()
{
    search_state_.ToggleHighlightEnabled();
    Invalidate();
}

void App::SetSearchSelection(int sel_start, int sel_end) noexcept
{
    if (search_caret_pos_ == sel_end && search_selection_start_ == sel_start) {
        return;
    }
    search_caret_pos_ = sel_end;
    search_selection_start_ = sel_start;
    if (search_has_focus_) {
        search_caret_visible_ = true;
        RestartSearchCaretBlink();
        InvalidateSearchBar();
    }
}

void App::SetImeComposition(std::wstring_view comp)
{
    if (ime_composition_ == comp) {
        return;
    }
    ime_composition_ = comp;
    if (search_has_focus_) {
        InvalidateSearchBar();
    }
}

RECT App::GetSearchEditRect() const
{
    if (!search_state_.IsVisible()) {
        return { 0, 0, 1, 1 };
    }
    const auto& layout = GetPaneLayout();
    const auto& r = layout.md_rect;
    const auto sbl = ComputeSearchBarLayout(r.x, r.width, r.y + r.height, !search_state_.GetQuery().empty());
    const float s = cached_dpi_scale_;
    return {
        static_cast<LONG>(sbl.input_rect.left * s),
        static_cast<LONG>(sbl.input_rect.top * s),
        static_cast<LONG>(sbl.input_rect.right * s),
        static_cast<LONG>(sbl.input_rect.bottom * s),
    };
}

void App::InvalidateSearchBar()
{
    const auto& layout = GetPaneLayout();
    const auto& r = layout.md_rect;
    const PaneRect search_area{ r.x, r.y + r.height - SEARCH_BAR_HEIGHT, r.width, SEARCH_BAR_HEIGHT };
    InvalidatePane(search_area);
}

void App::RestartSearchCaretBlink()
{
    KillTimer(hwnd_, TIMER_SEARCH_CARET);
    const UINT blink_time = GetCaretBlinkTime();
    if (blink_time > 0 && blink_time != INFINITE) {
        SetTimer(hwnd_, TIMER_SEARCH_CARET, blink_time, nullptr);
    }
}

void App::ScrollToCurrentMatch()
{
    const int idx = search_state_.GetCurrentMatchIndex();
    if (idx < 0 || idx >= search_state_.GetMatchCount()) {
        return;
    }
    const auto& match = search_state_.GetMatches()[idx];
    if (match.node_index < 0 || match.node_index >= static_cast<int>(layout_cache_.size())) {
        return;
    }

    const auto& entry = layout_cache_[match.node_index];
    const float match_y = entry.y_position;
    const auto& pane_layout = GetPaneLayout();
    const float viewport_height = pane_layout.md_rect.height;
    const float visible_height = viewport_height
        - (search_state_.IsVisible() ? SEARCH_BAR_HEIGHT : 0.0f);
    const float effective_bottom = viewport_.GetScrollY() + visible_height;
    const float scroll_y = viewport_.GetScrollY();

    // マッチが可視範囲外の場合のみスクロール
    if (match_y < scroll_y || match_y + entry.height > effective_bottom) {
        const float target = std::max(0.0f, match_y - visible_height / 3.0f);
        viewport_.SetScrollY(target);
        SyncMaxScroll(visible_height);
        InvalidateHitPositions();
    }
}

// ============================================================
// 最後に開いたファイルの永続化
// ============================================================

void App::SaveLastFilePath()
{
    if (doc_.GetFilePath().empty() || IsHelpPath(doc_.GetFilePath())) {
        return;
    }
    config_.SaveWString("Session", "LastFile", doc_.GetFilePath());
}

std::pmr::wstring App::LoadLastFilePath() const
{
    std::pmr::wstring path = config_.LoadWString("Session", "LastFile");
    if (path.empty()) {
        return {};
    }
    // 安全なローカルファイルパスであることを検証
    // UNCパス (\\server\...) やデバイスパス (\\.\, \\?\) をブロック
    if (path.size() >= 2 && path[0] == L'\\' && path[1] == L'\\') {
        return {};
    }
    if (GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES) {
        return {};
    }
    return path;
}

void App::ShowDirectory(std::wstring_view dir_path)
{
    file_explorer_.SetDirectory(dir_path);
    renderer_.InvalidateFilePaneCache();
    Invalidate();
}

// ============================================================
// ペイン状態の永続化
// ============================================================

void App::SavePaneState()
{
    config_.SaveBool("Pane", "ShowFile", panes_.IsFilePaneVisible());
    config_.SaveBool("Pane", "ShowToc", panes_.IsTocPaneVisible());
    config_.SaveInt("Pane", "FileWidth", static_cast<int>(std::lround(panes_.GetFilePaneWidth())));
    config_.SaveInt("Pane", "TocWidth", static_cast<int>(std::lround(panes_.GetTocPaneWidth())));
}

void App::LoadPaneState()
{
    panes_.SetFilePaneVisible(config_.LoadBool("Pane", "ShowFile", true));
    panes_.SetTocPaneVisible(config_.LoadBool("Pane", "ShowToc", true));

    constexpr int kDefaultWidth = static_cast<int>(PaneController::PANE_DEFAULT_WIDTH);
    constexpr int kMinWidth = static_cast<int>(PaneController::PANE_MIN_WIDTH);

    // クライアント幅に基づいて有効な最大ペイン幅を計算する
    int dynamic_max = kDefaultWidth;
    if (hwnd_) {
        RECT rc{};
        if (GetClientRect(hwnd_, &rc)) {
            const int client_width = rc.right - rc.left;
            if (client_width > 0) {
                dynamic_max = std::max(kMinWidth, client_width - kMinWidth);
            }
        }
    }

    panes_.SetFilePaneWidth(static_cast<float>(
        config_.LoadInt("Pane", "FileWidth", kDefaultWidth, kMinWidth, dynamic_max)));
    panes_.SetTocPaneWidth(static_cast<float>(
        config_.LoadInt("Pane", "TocWidth", kDefaultWidth, kMinWidth, dynamic_max)));
}

void App::SaveScrollPosition()
{
    const int node = FindFirstVisibleNode();
    if (node < 0) {
        return;
    }
    const float node_y = layout_cache_[node].y_position;
    const int offset = static_cast<int>(std::lround(viewport_.GetScrollY() - node_y));
    config_.SaveInt("Session", "ScrollNode", node);
    config_.SaveInt("Session", "ScrollOffset", offset);
    // 遅延レイアウト完了後に正確な位置を復元するための生のscroll_y
    config_.SaveInt("Session", "ScrollY", static_cast<int>(std::lround(viewport_.GetScrollY())));
}
