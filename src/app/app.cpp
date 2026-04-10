#include "app.h"
#include "app_constants.h"
#include "file_loader.h"
#include "parser.h"
#include "resource.h"
#include "i18n.h"
#include "pane_layout.h"
#include "document_utils.h"
#include "mermaid_util.h"
#include "layout.h"
#include "ui_constants.h"
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
    cached_dpi_scale_ = (init_dpi > 0.0f) ? (init_dpi / DEFAULT_DPI) : 1.0f;

    // タスクスケジューラを初期化（画像デコード・キャッシュ書き込み共用）
    scheduler_.Init(mermaid_util::ComputeWorkerCount(
        std::thread::hardware_concurrency()));

    file_cache_.Init(cached_dpi_scale_, scheduler_);
    mermaid_renderer_.SetFileCache(&file_cache_);

    resource_manager_.Init(doc_, layout_cache_, viewport_, image_loader_, mermaid_renderer_,
        theme_service_, renderer_, BuildResourceManagerCallbacks());

    mermaid_renderer_.Init(hwnd_, renderer_.GetRenderTarget(), [this]() {
        resource_manager_.ScheduleMermaidBatch();
    });

    image_loader_.Init(renderer_.GetRenderTarget(), renderer_.GetWICFactory());
    image_loader_.InitAsync(hwnd_, app_msg::IMAGE_LOADED, scheduler_);

    // D2Dデバイスロスト時にレンダーターゲットが再作成されたら、各ローダーを更新
    renderer_.SetDeviceLostCallback([this](ID2D1RenderTarget* new_rt) {
        mermaid_renderer_.SetRenderTarget(new_rt);
        image_loader_.CancelPending();
        image_loader_.SetRenderTarget(new_rt);
        image_loader_.ClearCache();
        resource_manager_.LoadImages();
    });

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

    cursors_.Init();

    {
        const auto* rt = renderer_.GetRenderTarget();
        const float window_w = rt ? rt->GetSize().width : 1600.0f;
        titlebar_.UpdateLayout(window_w);
    }

    LoadPaneState();

    ctx_menu_.Init(renderer_.GetD2DFactory(), renderer_.GetDWriteFactory());

    tooltip_.Init(hwnd_);
    if (theme_service_.IsDarkMode()) {
        tooltip_.ApplyDarkMode(true);
    }

    search_bar_ctrl_.Init(search_state_, viewport_, layout_cache_, BuildSearchBarCallbacks());

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
// ファイル読み込み/リロード共通ヘルパー
// ============================================================

void App::CancelPendingResources()
{
    resource_manager_.CancelMermaidBatch();
    image_loader_.CancelPending();
    resource_manager_.ClearResolvedPaths();
}

void App::FinalizeLayout(float md_pane_height)
{
    resource_manager_.LoadImages();
    resource_manager_.RequestMermaidRenders();
    SyncMaxScroll(md_pane_height);
    UpdateScrollBar();
    Invalidate();
    ScheduleDeferredLayoutIfNeeded();
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

// ============================================================
// 描画 / リサイズ
// ============================================================

void App::OnPaint()
{
    MENDO_PROFILE("OnPaint");

    PAINTSTRUCT ps;
    BeginPaint(hwnd_, &ps);

    const auto& layout = GetPaneLayout();
    // pending_prefix_shrink_ 中は loading_ が true でも旧コンテンツを表示する。
    // エディタの truncate→rewrite 中間状態をスキップしているだけなので、
    // ローディング画面を表示する必要がない。
    const bool show_loading = file_load_service_.IsLoading() && !pending_prefix_shrink_;
    if (!show_loading) {
        // 完了済みデコード結果とキャッシュ済みリソースを描画前に適用する。
        // app_msg::IMAGE_LOADED が WM_PAINT より後にキューされている場合でも
        // プレースホルダーの表示を回避できる。
        resource_manager_.FlushPendingResources();

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
    if (panes_.IsTocPaneVisible() && !show_loading) {
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
                auto& toc_scroll = panes_.TocScroll();
                toc_scroll.max_scroll = info.max_scroll;
                float& sy = toc_scroll.scroll_y;
                sy = std::clamp(sy, 0.0f, info.max_scroll);
                if (info.content_height > 0.0f) {
                    // フォーカスを表示領域の5等分中、区画2〜4(1/5〜4/5)に留める
                    const float zone_upper = info.content_height * (1.0f / 5.0f);
                    const float zone_lower = info.content_height * (4.0f / 5.0f);
                    if (item_y < sy + zone_upper) {
                        sy = std::clamp(item_y - zone_upper, 0.0f, info.max_scroll);
                    }
                    else if (item_y + theme.pane_item_height > sy + zone_lower) {
                        sy = std::clamp(item_y + theme.pane_item_height - zone_lower, 0.0f, info.max_scroll);
                    }
                }
            }
        }
    }

    const auto gs = BuildGestureRenderState();
    const auto sp = BuildSidePaneState(layout);
    const auto tb = BuildTitleBarRenderState(cached_window_width_for_layout_);
    const auto ts = BuildToastRenderState();
    const auto sb = BuildSearchBarRenderState();

    if (show_loading) {
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
                viewport_.GetSelection(), layout.md_rect, sp, tb, gs, ts, sb,
                viewport_.GetScrollY(), layout_service_->GetTotalHeight(),
                static_cast<int>(nav_hover_), hovered_copy_node_, hovered_save_node_,
                nav_service_.CanGoBack(), nav_service_.CanGoForward(),
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

    KillTimer(hwnd_, app_timer::LOADING_ANIM);
    file_load_service_.StopLoading();
    viewport_.ClearSelection();
    CancelPendingResources();
    renderer_.ShrinkBuffers();
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
    KillTimer(hwnd_, app_timer::FILE_RELOAD_DEBOUNCE);
    pending_prefix_shrink_ = false;
    const std::pmr::wstring path_str{ path };
    if (!DocumentService::NeedsLoadingAnimation(path_str)) {
        file_load_service_.SetLoadingPath(path_str);
        DoLoadMarkdownFile();
    }
    else {
        file_load_service_.StartLoading(path_str);
        SetTimer(hwnd_, app_timer::LOADING_ANIM, 16, nullptr);
        Invalidate();
        UpdateWindow(hwnd_);
        file_load_service_.StartAsyncLoad(scheduler_, hwnd_, app_msg::PARSE_COMPLETE);
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

    KillTimer(hwnd_, app_timer::LOADING_ANIM);

    {
        MENDO_PROFILE("ExecuteLoad(FileIO+Parse)");
        if (!file_load_service_.ExecuteLoad(doc_, layout_cache_)) {
            Invalidate();
            return;
        }
    }

    FinishLoadMarkdownFile();
}

void App::OnParseComplete()
{
    KillTimer(hwnd_, app_timer::LOADING_ANIM);
    file_load_service_.StopLoading();

    auto result = file_load_service_.TakeAsyncResult();
    if (!result) {
        MENDO_TRACE("OnParseComplete: no result (cancelled?)");
        Invalidate();
        return;
    }

    // 差分ベースのスキップ／スクロール復元は同一パスのリロード時のみ有効。
    // 非同期のファイルオープンでも OnParseComplete() が使われるため、
    // 別ファイル読み込み時は差分ロジックをスキップする。
    if (_wcsicmp(result->GetFilePath().c_str(), doc_.GetFilePath().c_str()) == 0) {
        const std::string_view old_view(doc_.GetRawUtf8());
        const std::string_view new_view(result->GetRawUtf8());
        const size_t diff_pos = FindFirstDifference(old_view, new_view);

        MENDO_TRACEF("OnParseComplete: reload node_count=%zu diff_pos=%zu old_size=%zu new_size=%zu",
            result->GetNodes().size(), diff_pos, old_view.size(), new_view.size());

        if (diff_pos == std::string_view::npos) {
            // 差分なし → リロード不要、監視再開
            pending_prefix_shrink_ = false;
            doc_service_.ResumeWatching();
            Invalidate();
            return;
        }

        // diff_pos が短い方の末尾と一致 = 片方がもう片方のprefixで、
        // ファイルの伸縮に過ぎない場合はスクロール位置を維持する
        const bool is_prefix_only = IsPrefixOnlyDiff(diff_pos, old_view.size(), new_view.size());

        // エディタの truncate→rewrite 2段階保存の前半（ファイル縮小）を検出。
        // doc_ を更新せず元のコンテンツを保持することで、次のリロードで
        // 「元コンテンツ vs 最終コンテンツ」の正確な差分を検出できるようにする。
        if (ShouldDeferForTruncateRewrite(is_prefix_only, old_view.size(), new_view.size())) {
            return;
        }

        if (is_prefix_only) {
            // prefix-only はファイル末尾の伸縮のみ。prefix 部分のノード列は
            // 同一なので、旧キャッシュの実測高さ・text_layout をそのまま保持する。
            // Reset() + EstimateNodeHeights() だと推定高さの累積誤差で
            // 大規模ファイル後半のスクロール位置が大きくずれる。
            resource_manager_.CancelMermaidBatch();
            doc_ = std::move(*result);
            layout_cache_.ResizePreservingPrefix(doc_.GetNodes().size());

            renderer_.InvalidateTocPaneCache();

            const auto pane_layout = GetPaneLayout();
            const float md_width = pane_layout.md_rect.width;
            const float md_height = pane_layout.md_rect.height;

            viewport_.SetScrollY(reload_old_scroll_);
            layout_service_->ViewportLayout(doc_, layout_cache_, md_width, md_height);

            FinalizeLayout(md_height);

            if (search_state_.IsVisible() && !search_state_.GetQuery().empty()) {
                search_bar_ctrl_.RunSearchAndLocate(doc_.GetNodes());
            }

            doc_service_.ResumeWatching();
            return;
        }

        reload_diff_pos_ = diff_pos;
    }
    else {
        // 別ファイルの非同期ロードではリロード用の差分スクロールを使わない
        reload_diff_pos_ = std::string_view::npos;
    }

    doc_ = std::move(*result);
    layout_cache_.Reset(doc_.GetNodes().size());

    FinishLoadMarkdownFile();
}

void App::FinishLoadMarkdownFile()
{
    viewport_.ClearSelection();
    search_bar_ctrl_.Reset();
    PostMessage(hwnd_, app_msg::SEARCH_UNFOCUS, app_param::SEARCH_UNFOCUS_FILE_SWITCH, 0);
    active_toc_index_ = -1;
    CancelPendingResources();
    renderer_.ShrinkBuffers();

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

    const bool has_reload_diff = (reload_diff_pos_ != std::string_view::npos);

    MENDO_TRACEF("FinishLoad: has_reload_diff=%d HasNodeRestore=%d HasNavScroll=%d nav_scroll_y=%.1f",
        has_reload_diff ? 1 : 0,
        scroll_restore_.HasNodeRestore() ? 1 : 0,
        scroll_restore_.HasNavScroll() ? 1 : 0,
        scroll_restore_.HasNavScroll() ? scroll_restore_.pending_nav_scroll_y : -1.0f);

    // cache.Reset()直後は全ノードの高さが0のため、スクロール復元前に
    // ノード高さを推定し、Mermaidキャッシュの実測値で補正する
    if (has_reload_diff
        || scroll_restore_.HasNodeRestore()
        || (scroll_restore_.HasNavScroll() && scroll_restore_.pending_nav_scroll_y > 0.0f)) {
        MENDO_PROFILE("EstimateNodeHeights");
        EstimateNodeHeights(doc_.GetNodes(), layout_cache_, renderer_.GetTheme());
        ApplyMermaidCacheHeights(md_width);
    }

    if (has_reload_diff) {
        scroll_y = CalcScrollForDiff(reload_diff_pos_, md_height, reload_old_scroll_);
        reload_diff_pos_ = std::string_view::npos;
    }
    else if (scroll_restore_.HasNodeRestore()) {
        const int node = std::min(scroll_restore_.pending_restore_node,
            static_cast<int>(layout_cache_.size()) - 1);
        if (node >= 0) {
            scroll_y = std::max(0.0f,
                layout_cache_[node].y_position + static_cast<float>(scroll_restore_.pending_restore_offset));
        }
        scroll_restore_.ClearNodeRestore();
    }
    else if (scroll_restore_.HasNavScroll()) {
        scroll_y = scroll_restore_.ConsumeNavScroll();
        // 遅延レイアウトのドリフト補正用（セッション復元と同じ仕組み）
        scroll_restore_.pending_restore_scroll_y = scroll_y;
    }
    else {
        // 新規ファイルオープン: 前回ナビゲーションの残留値をクリア
        scroll_restore_.pending_restore_scroll_y = -1;
    }

    MENDO_TRACEF("FinishLoad: scroll_y=%.1f (0=top of file)", scroll_y);

    viewport_.SetScrollY(scroll_y);

    // 推定→計測の高さ差をアンカー補償
    const bool need_anchor = (scroll_y > 0.0f);
    const auto anchor = need_anchor ? SaveAnchor() : AnchorState{};

    {
        MENDO_PROFILE("ViewportLayout(Initial)");
        layout_service_->ViewportLayout(doc_, layout_cache_, md_width, md_height);
    }

    if (need_anchor) {
        viewport_.AnchorCompensateScroll(anchor.idx, anchor.y_before, layout_cache_);
    }

    FinalizeLayout(md_height);

    UpdateTitleBar();

    doc_service_.StartWatching(doc_.GetFilePath(), [this]() {
        KillTimer(hwnd_, app_timer::FILE_RELOAD_DEBOUNCE);
        SetTimer(hwnd_, app_timer::FILE_RELOAD_DEBOUNCE, 200, nullptr);
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
        MENDO_TRACE("ReloadCurrentFile: async path (large file)");
        reload_old_scroll_ = viewport_.GetScrollY();
        file_load_service_.StartLoading(doc_.GetFilePath());
        // pending_prefix_shrink_ 中はローディングアニメーションを表示しない。
        // エディタの truncate→rewrite の中間状態をスキップしているだけなので、
        // 画面を白くして再描画する必要がない。
        if (!pending_prefix_shrink_) {
            SetTimer(hwnd_, app_timer::LOADING_ANIM, 16, nullptr);
            Invalidate();
            UpdateWindow(hwnd_);
        }
        file_load_service_.StartAsyncLoad(scheduler_, hwnd_, app_msg::PARSE_COMPLETE);
    }
    else {
        MENDO_TRACE("ReloadCurrentFile: sync path (DoReloadCurrentFile)");
        DoReloadCurrentFile();
    }
}

void App::DoReloadCurrentFile()
{
    MENDO_PROFILE("DoReloadCurrentFile");

    KillTimer(hwnd_, app_timer::LOADING_ANIM);
    file_load_service_.StopLoading();
    active_toc_index_ = -1;

    if (doc_.GetFilePath().empty()) {
        return;
    }

    const float old_scroll = viewport_.GetScrollY();

    CancelPendingResources();

    // ファイルを読み込み、旧コンテンツとの差分位置をコピーなしで計算
    auto load_result = [this]() {
        MENDO_PROFILE("Reload::LoadFile");
        return FileLoader::LoadFile(doc_.GetFilePath());
    }();
    if (!load_result) {
        doc_service_.ResumeWatching();
        return;
    }
    std::pmr::string new_utf8 = std::move(*load_result);
    const std::string_view old_view(doc_.GetRawUtf8());
    const std::string_view new_view(new_utf8);
    const size_t diff_pos = FindFirstDifference(old_view, new_view);

    MENDO_TRACEF("DoReload: diff_pos=%zu old_size=%zu new_size=%zu old_scroll=%.1f",
        diff_pos, old_view.size(), new_view.size(), old_scroll);

    // 差分がなければリロード不要。エディタの保存操作が複数の通知を
    // 発生させた場合に、レイアウトキャッシュの不要なリセットを防ぐ。
    if (diff_pos == std::string_view::npos) {
        pending_prefix_shrink_ = false;
        doc_service_.ResumeWatching();
        return;
    }

    // 変更箇所のスクロール位置を決定
    // diff_pos が短い方の末尾と一致 = 片方がもう片方のprefixで、
    // ファイルの伸縮に過ぎない場合はスクロール位置を維持する
    const bool is_prefix_only = IsPrefixOnlyDiff(diff_pos, old_view.size(), new_view.size());

    // エディタの truncate→rewrite 2段階保存の前半（ファイル縮小）を検出。
    // doc_ を更新せず元のコンテンツを保持し、次のリロードで正確な差分を検出する。
    if (ShouldDeferForTruncateRewrite(is_prefix_only, old_view.size(), new_view.size())) {
        return;
    }

    // ドキュメントを新コンテンツで更新
    {
        MENDO_PROFILE("Reload::ReplaceFromMarkdown");
        doc_.ReplaceFromMarkdown(std::move(new_utf8));
    }

    if (is_prefix_only) {
        // prefix 部分のノード列は同一なので旧キャッシュの実測高さを保持する
        layout_cache_.ResizePreservingPrefix(doc_.GetNodes().size());
    }
    else {
        layout_cache_.Reset(doc_.GetNodes().size(), false);
        EstimateNodeHeights(doc_.GetNodes(), layout_cache_, renderer_.GetTheme());
    }

    renderer_.InvalidateTocPaneCache();

    const auto pane_layout = GetPaneLayout();
    const float md_width = pane_layout.md_rect.width;
    const float md_height = pane_layout.md_rect.height;

    if (!is_prefix_only) {
        // Mermaidブロックの推定高さをファイルキャッシュの実測値で上書きし、
        // スクロール位置のずれを防ぐ
        ApplyMermaidCacheHeights(md_width);
    }

    const float desired_scroll = is_prefix_only
        ? old_scroll
        : CalcScrollForDiff(diff_pos, md_height, old_scroll);

    MENDO_TRACEF("DoReload: desired_scroll=%.1f old_scroll=%.1f", desired_scroll, old_scroll);

    // スクロール位置を設定してからViewportLayoutを呼ぶことで、
    // 変更箇所周辺の可視ノードが計測される
    viewport_.SetScrollY(desired_scroll);

    {
        MENDO_PROFILE("Reload::ViewportLayout");
        layout_service_->ViewportLayout(doc_, layout_cache_, md_width, md_height);
    }

    FinalizeLayout(md_height);

    // 検索バー表示中なら再検索
    if (search_state_.IsVisible() && !search_state_.GetQuery().empty()) {
        search_bar_ctrl_.RunSearchAndLocate(doc_.GetNodes());
    }

    // リロード完了まで一時停止していたファイル監視を再開する
    doc_service_.ResumeWatching();
}

bool App::ShouldDeferForTruncateRewrite(bool is_prefix_only, size_t old_size, size_t new_size)
{
    if (is_prefix_only && new_size < old_size) {
        // prefix-only shrink が連続する場合もすべて defer する。
        // エディタによっては truncate が複数回発生してから rewrite されることがある。
        pending_prefix_shrink_ = true;
        doc_service_.ResumeWatching();
        return true;
    }
    pending_prefix_shrink_ = false;
    return false;
}

float App::CalcScrollForDiff(size_t diff_pos, float viewport_height, float fallback_scroll) const
{
    MENDO_TRACEF("CalcScrollForDiff: diff_pos=%zu node_count=%zu", diff_pos, doc_.GetNodes().size());
    return CalcScrollYForDiff(
        doc_.GetNodes(), layout_cache_,
        std::string_view(doc_.GetRawUtf8()),
        diff_pos, viewport_height, fallback_scroll);
}

void App::ApplyMermaidCacheHeights(float md_width)
{
    const float content_width = renderer_.GetTheme().ContentWidth(md_width);
    const bool dark_mode = theme_service_.IsDarkMode();
    const auto& nodes = doc_.GetNodes();
    bool any_applied = false;
    for (size_t i : doc_.GetMermaidNodeIndices()) {
        const auto hash = mermaid_util::HashCode(nodes[i].text_utf8, content_width, dark_mode);
        MermaidFileCache::CacheEntry fentry;
        if (file_cache_.LookupDimensions(hash, fentry)) {
            layout_cache_[i].height = fentry.css_height;
            any_applied = true;
        }
    }
    if (any_applied) {
        RecomputeYPositions(doc_.GetNodesMut(), layout_cache_, renderer_.GetTheme());
    }
}

void App::UpdateTitleBar()
{
    const int zoom_percent = static_cast<int>(ZOOM_STEPS[viewport_.GetZoomIndex()] * 100.0f + 0.5f);
    auto title = BuildTitleString(doc_.GetFilePath(), zoom_percent);
    SetWindowTextW(hwnd_, title.c_str());
    cached_title_text_ = std::move(title);
    InvalidateTitleBar();
}

// OnAppImageLoaded / OnAppReloadFile はWM_APPメッセージ経由でresource_manager_に委譲
void App::OnAppImageLoaded()
{
    resource_manager_.OnAppImageLoaded();
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
            KillTimer(hwnd_, app_timer::SWIPE_OVERLAY);
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
    SetTimer(hwnd_, app_timer::SWIPE_OVERLAY, static_cast<UINT>(SwipeDetector::COMMIT_TIMEOUT_MS), nullptr);

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
                scroll_restore_.pending_restore_scroll_y = -1;
                const float old_scroll = viewport_.GetScrollY();
                const auto pane_layout = GetPaneLayout();
                const float page_size = pane_layout.md_rect.height;
                switch (a.type) {
                    case ScrollType::LineUp:   viewport_.DirectScrollBy(-SCROLL_LINE_AMOUNT); break;
                    case ScrollType::LineDown: viewport_.DirectScrollBy(SCROLL_LINE_AMOUNT); break;
                    case ScrollType::PageUp:   viewport_.DirectScrollBy(-page_size * SCROLL_PAGE_FACTOR); break;
                    case ScrollType::PageDown: viewport_.DirectScrollBy(page_size * SCROLL_PAGE_FACTOR); break;
                    case ScrollType::Home:     viewport_.ScrollTo(0.0f); break;
                    case ScrollType::End:      viewport_.ScrollTo(viewport_.GetMaxScroll()); break;
                    default:                   break;
                }
                if (viewport_.GetScrollY() != old_scroll) {
                    InvalidateHitPositions();
                    Invalidate();
                    resource_manager_.ScheduleBitmapManage();
                }
            },
            [this](const DirectScrollByAction& a) {
                scroll_restore_.pending_restore_scroll_y = -1;
                viewport_.DirectScrollBy(a.delta);
                InvalidateHitPositions();
                Invalidate();
                resource_manager_.ScheduleBitmapManage();
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
                if (search_bar_ctrl_.GetState().IsVisible()) {
                    search_bar_ctrl_.OnClose();
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
                search_bar_ctrl_.OnOpen(doc_.GetNodes());
            },
            [this](const CloseSearchBarAction&) {
                search_bar_ctrl_.OnClose();
            },
            [this](const SearchNextAction&) {
                if (!search_bar_ctrl_.GetState().IsVisible()) {
                    search_bar_ctrl_.OnOpen(doc_.GetNodes());
                }
                else {
                    search_bar_ctrl_.OnNext();
                }
            },
            [this](const SearchPrevAction&) {
                if (!search_bar_ctrl_.GetState().IsVisible()) {
                    search_bar_ctrl_.OnOpen(doc_.GetNodes());
                }
                else {
                    search_bar_ctrl_.OnPrev();
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

void App::OnFileWatchEvent()
{
    MENDO_TRACE("OnFileWatchEvent: file change detected");
    doc_service_.CheckForChanges();
}

void App::HandleTimer(UINT_PTR timer_id)
{
    MENDO_PROFILE("HandleTimer");
    switch (timer_id) {
    case app_timer::DEFERRED_LAYOUT: OnDeferredLayout(); break;
    case app_timer::LOADING_ANIM:
        file_load_service_.TickLoadingAnimation();
        Invalidate();
        break;
    case app_timer::SWIPE_OVERLAY: {
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
        KillTimer(hwnd_, app_timer::SWIPE_OVERLAY);
        if (need_redraw) {
            Invalidate();
        }
        break;
    }
    case app_timer::TOAST: {
        if (!toast_.Tick()) {
            KillTimer(hwnd_, app_timer::TOAST);
        }
        Invalidate();
        break;
    }
    case app_timer::SEARCH_CARET: {
        search_bar_ctrl_.OnCaretBlinkTimer();
        break;
    }
    case app_timer::TOOLTIP:
        KillTimer(hwnd_, app_timer::TOOLTIP);
        tooltip_.Show();
        break;
    case app_timer::SEARCH_DEBOUNCE:
        search_bar_ctrl_.OnDebounceTimer(doc_.GetNodes());
        break;
    case app_timer::MERMAID_BATCH:
        resource_manager_.ProcessMermaidBatch();
        break;
    case app_timer::BITMAP_MANAGE:
        resource_manager_.OnBitmapManageTimer();
        break;
    case app_timer::MERMAID_INIT_RETRY:
        mermaid_renderer_.OnInitRetryTimer();
        break;
    case app_timer::FILE_RELOAD_DEBOUNCE:
        KillTimer(hwnd_, app_timer::FILE_RELOAD_DEBOUNCE);
        ReloadCurrentFile();
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
    search_bar_ctrl_.OnCaptureChanged();
    if (gesture_.GetPhase() != GesturePhase::Idle) {
        gesture_.Reset();
        Invalidate();
    }
}

void App::ShowToast(std::wstring_view message)
{
    toast_.Show(message);
    SetTimer(hwnd_, app_timer::TOAST, 16, nullptr);
    Invalidate();
}

void App::OnDestroy()
{
    // メッセージループが生きているうちにWebView2を閉じる。
    // デストラクタではメッセージループが停止済みのため、
    // WebView2のClose()がブロックしてハングする。
    mermaid_renderer_.Shutdown();

    // スケジューラを停止してキュー済み書き込みを完了させた後、
    // file_cacheのscheduler_をnullにして遅延COMコールバックからの
    // 新規ポストを防ぐ。
    scheduler_.Shutdown();
    file_cache_.Shutdown();
    file_cache_.SaveIndex();
    SaveLastFilePath();
    SavePaneState();
    SaveScrollPosition();
    config_.SaveWString("General", "Language", i18n::GetLangKey());
    for (UINT_PTR id : {
        app_timer::DEFERRED_LAYOUT,
        app_timer::LOADING_ANIM,
        app_timer::SWIPE_OVERLAY,
        app_timer::TOAST,
        app_timer::SEARCH_CARET,
        app_timer::SEARCH_DEBOUNCE,
        app_timer::TOOLTIP,
        app_timer::BITMAP_MANAGE,
        app_timer::MERMAID_BATCH,
        app_timer::MERMAID_INIT_RETRY,
        app_timer::FILE_RELOAD_DEBOUNCE,
    }) {
        KillTimer(hwnd_, id);
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

// ============================================================
// 最後に開いたファイルの永続化
// ============================================================

void App::SaveLastFilePath()
{
    if (!IsHelpPath(doc_.GetFilePath())) {
        session_.SaveLastFilePath(doc_.GetFilePath());
    }
}

std::pmr::wstring App::LoadLastFilePath() const
{
    return session_.LoadLastFilePath();
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
    session_.SavePaneState(panes_);
}

void App::LoadPaneState()
{
    float client_width = 0.0f;
    if (hwnd_) {
        RECT rc{};
        if (GetClientRect(hwnd_, &rc)) {
            client_width = static_cast<float>(rc.right - rc.left);
        }
    }
    session_.LoadPaneState(panes_, client_width);
}

void App::SaveScrollPosition()
{
    const int node = FindFirstVisibleNode();
    if (node < 0) {
        return;
    }
    session_.SaveScrollPosition(node, viewport_.GetScrollY(), layout_cache_[node].y_position);
}

// ============================================================
// Init用コールバック構築
// ============================================================

ResourceManager::Callbacks App::BuildResourceManagerCallbacks()
{
    return {
        .invalidate = [this]() { Invalidate(); },
        .set_timer = [this](UINT_PTR id, UINT ms) { SetTimer(hwnd_, id, ms, nullptr); },
        .kill_timer = [this](UINT_PTR id) { KillTimer(hwnd_, id); },
        .get_content_width = [this]() -> float {
            return renderer_.GetTheme().ContentWidth(GetMarkdownPaneWidth());
        },
        .get_viewport_height = [this]() -> float {
            return GetPaneLayout().md_rect.height;
        },
        .recompute_layout = [this]() {
            layout_service_->RecomputeAfterDiagram(doc_, layout_cache_, renderer_.GetTheme());
        },
        .recompute_layout_anchored = [this]() {
            const auto anchor = SaveAnchor();
            layout_service_->RecomputeAfterDiagram(doc_, layout_cache_, renderer_.GetTheme());
            const auto layout = GetPaneLayout();
            RestoreAnchor(anchor, layout.md_rect.height);
            Invalidate();
        },
    };
}

SearchBarController::Callbacks App::BuildSearchBarCallbacks()
{
    return {
        .invalidate = [this]() { Invalidate(); },
        .invalidate_search_bar = [this]() {
            const auto& layout = GetPaneLayout();
            const auto& r = layout.md_rect;
            const PaneRect search_area{ r.x, r.y + r.height - SEARCH_BAR_HEIGHT, r.width, SEARCH_BAR_HEIGHT };
            InvalidatePane(search_area);
        },
        .set_timer = [this](UINT_PTR id, UINT ms) { SetTimer(hwnd_, id, ms, nullptr); },
        .kill_timer = [this](UINT_PTR id) { KillTimer(hwnd_, id); },
        .focus_select_all = [this]() {
            PostMessage(hwnd_, app_msg::SEARCH_FOCUS, app_param::SEARCH_FOCUS_SELECT_ALL, 0);
        },
        .focus_set_caret = [this](int pos) {
            PostMessage(hwnd_, app_msg::SEARCH_FOCUS, app_param::SEARCH_FOCUS_SET_CARET, static_cast<LPARAM>(pos));
        },
        .focus_set_selection = [this](int anchor, int caret) {
            PostMessage(hwnd_, app_msg::SEARCH_FOCUS, app_param::SEARCH_FOCUS_SET_SELECTION, MAKELPARAM(anchor, caret));
        },
        .unfocus = [this]() {
            PostMessage(hwnd_, app_msg::SEARCH_UNFOCUS, 0, 0);
        },
        .get_md_pane_height = [this]() -> float {
            return GetPaneLayout().md_rect.height;
        },
        .on_scroll_changed = [this](float visible_h) {
            SyncMaxScroll(visible_h);
            InvalidateHitPositions();
        },
    };
}
