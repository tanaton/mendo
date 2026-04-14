#include "app.h"
#include "app_constants.h"
#include "render_composer.h"
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

    layout_service_.emplace(renderer_.GetLayout(), state_.viewport);

    // PixelToDip用にDPIスケールをキャッシュ (OnDpiChangedで更新)
    const float init_dpi = static_cast<float>(GetDpiForWindow(hwnd_));
    state_.cached_dpi_scale = (init_dpi > 0.0f) ? (init_dpi / DEFAULT_DPI) : 1.0f;

    // タスクスケジューラを初期化（画像デコード・キャッシュ書き込み共用）
    scheduler_.Init(mermaid_util::ComputeWorkerCount(
        std::thread::hardware_concurrency()));

    file_cache_.Init(state_.cached_dpi_scale, scheduler_);
    mermaid_renderer_.SetFileCache(&file_cache_);

    resource_manager_.Init(state_.doc, state_.layout_cache, state_.viewport, image_loader_, mermaid_renderer_,
        theme_service_, renderer_, BuildResourceManagerCallbacks());
    effect_executor_.Init(hwnd_, resource_manager_, cursors_, doc_service_, config_,
        state_, *layout_service_, {
            .load_file = [this](std::wstring_view path) { LoadMarkdownFile(path); },
            .reload_file = [this]() { ReloadCurrentFile(); },
            .open_file_dialog = [this]() {
                const auto path = FileLoader::OpenFileDialog(hwnd_);
                if (!path.empty()) {
                    if (!state_.doc.GetFilePath().empty()) {
                        PushNavHistory();
                    }
                    LoadMarkdownFile(path);
                }
            },
            .invalidate_pane_cache = [this](PaneZone pane) {
                if (pane == PaneZone::FilePane) {
                    renderer_.InvalidateFilePaneCache();
                }
                else if (pane == PaneZone::TocPane) {
                    renderer_.InvalidateTocPaneCache();
                }
            },
            .refresh_pane_layout = [this]() {
                RefreshPaneLayout();
            },
            .renderer_resize = [this](UINT w, UINT h) {
                renderer_.Resize(w, h);
            },
            .renderer_set_dpi = [this](float dpi) {
                renderer_.SetDpi(dpi);
            },
            .clear_file_cache = [this]() {
                file_cache_.ClearAll();
            },
            .perform_resize_end = [this]() {
                OnResizeEnd();
            },
            .perform_sizing_update = [this]() {
                const auto& sizing_layout = GetPaneLayout();
                SyncMaxScroll(sizing_layout.md_rect.height);
                UpdateScrollBar();
                Invalidate();
            },
            .apply_theme_change = [this](const effect::ApplyThemeChange& e) {
                HandleApplyThemeChange(e);
            },
            .process_deferred_layout = [this]() {
                OnDeferredLayout();
            },
            .tick_loading_animation = [this]() {
                file_load_service_.TickLoadingAnimation();
            },
            .process_mermaid_batch_timer = [this]() {
                resource_manager_.ProcessMermaidBatch();
            },
            .process_bitmap_manage = [this]() {
                resource_manager_.OnBitmapManageTimer();
            },
            .mermaid_init_retry = [this]() {
                mermaid_renderer_.OnInitRetryTimer();
            },
            .destroy = [this]() {
                OnDestroy();
            },
            .handle_parse_complete = [this]() {
                OnParseComplete();
            },
            .handle_mouse_event = [this](effect::MouseEventType type, int px, int py) {
                switch (type) {
                case effect::MouseEventType::LButtonDown:  OnLButtonDown(px, py);  break;
                case effect::MouseEventType::LButtonUp:    OnLButtonUp(px, py);    break;
                case effect::MouseEventType::MouseMove:    OnMouseMove(px, py);    break;
                case effect::MouseEventType::MouseHover:   OnMouseHover(px, py);   break;
                case effect::MouseEventType::LButtonDblClk: OnLButtonDblClk(px, py); break;
                case effect::MouseEventType::RButtonDown:  OnRButtonDown(px, py);  break;
                case effect::MouseEventType::RButtonUp:    OnRButtonUp(px, py);    break;
                case effect::MouseEventType::RButtonMove:  OnRButtonMove(px, py);  break;
                }
            },
            .handle_context_menu = [this](int sx, int sy) {
                OnContextMenu(sx, sy);
            },
        }
    );

    mermaid_renderer_.Init(hwnd_, renderer_.GetRenderTarget(), renderer_.GetWICFactory(), [this]() {
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
    state_.viewport.SetZoomIndex(theme_service_.LoadZoomIndex());
    if (theme_service_.IsDarkMode() || state_.viewport.GetZoomIndex() != ZOOM_DEFAULT_INDEX) {
        renderer_.SetTheme(theme_service_.CreateTheme(state_.viewport.GetZoomIndex()));
        if (state_.viewport.GetZoomIndex() != ZOOM_DEFAULT_INDEX) {
            state_.panes.ApplyZoom(state_.viewport.GetCurrentZoom());
        }
    }
    if (theme_service_.IsDarkMode()) {
        ApplyDarkModeToWindow(hwnd_, true);
    }

    SyncPaneThemeCache();

    cursors_.Init();

    {
        const auto* rt = renderer_.GetRenderTarget();
        const float window_w = rt ? rt->GetSize().width : 1600.0f;
        state_.titlebar.UpdateLayout(window_w);
    }

    LoadPaneState();

    state_.ctx_menu.Init(renderer_.GetD2DFactory(), renderer_.GetDWriteFactory());

    state_.tooltip.Init(hwnd_);
    if (theme_service_.IsDarkMode()) {
        state_.tooltip.ApplyDarkMode(true);
    }

    state_.search_bar_ctrl.Init(state_.search_state, state_.viewport, state_.layout_cache, BuildSearchBarCallbacks());

    return true;
}

// ============================================================
// ヘルパー
// ============================================================

App::DipPoint App::PixelToDip(int px, int py) const noexcept
{
    return { px / state_.cached_dpi_scale, py / state_.cached_dpi_scale };
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
        state_.panes.SetDragScrollOffset(dip_y - thumb_y);
    }
    else {
        state_.panes.SetDragScrollOffset(info.thumb_height * 0.5f);
        const float new_thumb_y = dip_y - state_.panes.GetDragScrollOffset();
        scroll.scroll_y = ScrollFromThumbY(info, new_thumb_y);
        scroll.max_scroll = info.max_scroll;
        cache_dirty = true;
        Invalidate();
    }
}

void App::HandleScrollbarDrag(float dip_y, const PaneScrollInfo& info,
    ScrollState& scroll, bool& cache_dirty)
{
    const float new_thumb_y = dip_y - state_.panes.GetDragScrollOffset();
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
// ペインレイアウト
// ============================================================

const PaneLayout& App::GetPaneLayout() const
{
    if (!state_.pane_layout_valid) {
        auto* rt = renderer_.GetRenderTarget();
        if (!rt) {
            static const PaneLayout empty{};
            return empty;
        }
        const auto size = rt->GetSize();
        state_.cached_window_width_for_layout = size.width;
        const float tb_h = state_.titlebar.GetHeight();
        state_.cached_pane_layout = state_.panes.ComputeLayout(size.width, size.height,
            renderer_.GetTheme().splitter_width, tb_h);
        state_.pane_layout_valid = true;
    }
    return state_.cached_pane_layout;
}

void App::InvalidatePane(const PaneRect& rect) noexcept
{
    const float scale = state_.cached_dpi_scale;
    RECT rc;
    rc.left = static_cast<LONG>(rect.x * scale);
    rc.top = static_cast<LONG>(rect.y * scale);
    rc.right = static_cast<LONG>((rect.x + rect.width) * scale) + 1;
    rc.bottom = static_cast<LONG>((rect.y + rect.height) * scale) + 1;
    InvalidateRect(hwnd_, &rc, FALSE);
}

void App::InvalidateTitleBar() noexcept
{
    const float tb_h = state_.titlebar.GetHeight();
    if (tb_h <= 0.0f) {
        return;
    }
    // 幅が未計算（初期化直後など）の場合はウィンドウ全体を無効化する
    if (state_.cached_window_width_for_layout <= 0.0f) {
        InvalidateRect(hwnd_, nullptr, FALSE);
        return;
    }
    InvalidatePane(PaneRect{ 0.0f, 0.0f, state_.cached_window_width_for_layout, tb_h });
}

PaneZone App::PaneAtPoint(float dip_x, [[maybe_unused]] float dip_y) const
{
    const auto* rt = renderer_.GetRenderTarget();
    if (!rt) {
        return PaneZone::None;
    }
    const auto size = rt->GetSize();
    return state_.panes.DetectZone(dip_x, size.width, size.height,
        renderer_.GetTheme().splitter_width);
}

float App::GetMarkdownPaneWidth() const
{
    const auto layout = GetPaneLayout();
    return layout.md_rect.width;
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
    // state_.pending_prefix_shrink 中は loading_ が true でも旧コンテンツを表示する。
    // エディタの truncate→rewrite 中間状態をスキップしているだけなので、
    // ローディング画面を表示する必要がない。
    const bool show_loading = file_load_service_.IsLoading() && !state_.pending_prefix_shrink;
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
                state_.doc, state_.layout_cache, layout.md_rect.width, layout.md_rect.height);
        }

        if (updated) {
            RestoreAnchor(anchor, layout.md_rect.height);
        }
    }
    // 目次ペインの同期: mdペインのスクロール位置からアクティブ見出しを判定し、
    // 目次ペインを自動スクロールする
    if (state_.panes.IsTocPaneVisible() && !show_loading) {
        const float toc_margin = layout.md_rect.y + renderer_.GetTheme().heading_spacing_above;
        const int new_active = state_.doc.GetToc().FindActiveIndex(state_.layout_cache, state_.viewport.GetScrollY(), toc_margin);
        if (new_active != state_.active_toc_index) {
            state_.active_toc_index = new_active;
            renderer_.InvalidateTocPaneCache();

            // アクティブ見出しが目次ペインの表示範囲外なら自動スクロール
            if (new_active >= 0) {
                const auto& theme = renderer_.GetTheme();
                const float item_y = static_cast<float>(new_active) * theme.pane_item_height;
                const float total = static_cast<float>(state_.doc.GetToc().GetEntries().size()) * theme.pane_item_height;
                const auto info = ComputeScrollInfo(layout.toc_rect, theme.pane_header_height, total);
                auto& toc_scroll = state_.panes.TocScroll();
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

    const auto gs = render_composer::BuildGestureState(state_);
    const auto sp = render_composer::BuildSidePaneState(state_, layout);
    const auto tb = render_composer::BuildTitleBarState(state_,
        state_.cached_window_width_for_layout,
        theme_service_.IsDarkMode(),
        IsZoomed(hwnd_) != FALSE);
    const auto ts = render_composer::BuildToastState(state_);
    const auto sb = render_composer::BuildSearchBarState(state_);

    if (show_loading) {
        renderer_.DrawLoading(file_load_service_.GetLoadingAngle(), layout.md_rect, sp, tb, gs, ts);
    }
    else {
        // 検索マッチ情報をコマンドジェネレータに設定
        if (state_.search_state.IsVisible() && state_.search_state.IsHighlightEnabled() && !state_.search_state.GetMatches().empty()) {
            renderer_.SetSearchMatches(&state_.search_state.GetMatches(), state_.search_state.GetCurrentMatchIndex());
        }
        else {
            renderer_.SetSearchMatches(nullptr, -1);
        }

        // 描画前パス: 可視ノードに描画エフェクトを適用（Render の前に実行）
        renderer_.PrepareVisibleEffects(
            state_.doc.GetNodesMut(), state_.layout_cache,
            state_.viewport.GetScrollY(), layout.md_rect.height);

        {
            MENDO_PROFILE("Renderer::Render");
            renderer_.Render({
                state_.doc.GetNodes(), state_.layout_cache,
                state_.viewport.GetSelection(), layout.md_rect, sp, tb, gs, ts, sb,
                state_.viewport.GetScrollY(), layout_service_->GetTotalHeight(),
                static_cast<int>(state_.nav_hover), state_.hovered_copy_node, state_.hovered_save_node,
                state_.nav_history.CanGoBack(), state_.nav_history.CanGoForward(),
                layout_service_->HasDirtyNodes()
                });
        }
    }

    EndPaint(hwnd_, &ps);
}

void App::OnResize(UINT width, UINT height)
{
    Dispatch(ResizeAction{ width, height });
}

void App::OnDpiChanged(UINT dpi, const RECT* suggested)
{
    Dispatch(DpiChangedAction{ dpi, *suggested });
}

// ============================================================
// ファイル読み込み
// ============================================================

void App::LoadHelpDocument()
{
    if (IsHelpPath(state_.doc.GetFilePath())) {
        return;
    }

    const auto rc = LoadRcData(i18n::S().help_resource_id);
    if (rc.empty()) {
        return;
    }

    KillTimer(hwnd_, app_timer::LOADING_ANIM);
    file_load_service_.StopLoading();
    state_.viewport.ClearSelection();
    CancelPendingResources();
    renderer_.ShrinkBuffers();
    doc_service_.StopWatching();

    std::pmr::string utf8(reinterpret_cast<const char*>(rc.data()), rc.size());
    state_.doc = Document::FromMarkdown(std::move(utf8), HELP_PATH);
    state_.layout_cache.Reset(state_.doc.GetNodes().size());

    state_.file_explorer.SetCurrentFile(L"");
    renderer_.InvalidateFilePaneCache();

    state_.panes.ResetScrollStates();
    renderer_.InvalidateTocPaneCache();

    // ビューポート優先レイアウト + 遅延処理
    state_.viewport.SetScrollY(0.0f);
    {
        const auto pane_layout = GetPaneLayout();
        layout_service_->ViewportLayout(state_.doc, state_.layout_cache,
            pane_layout.md_rect.width, pane_layout.md_rect.height);
        SyncMaxScroll(pane_layout.md_rect.height);
    }
    UpdateScrollBar();
    Invalidate();
    ScheduleDeferredLayoutIfNeeded();

    UpdateTitleBar();
}

void App::BeginAsyncLoad(const std::pmr::wstring& path)
{
    file_load_service_.SetLoadingPath(path);
    if (DocumentService::NeedsLoadingAnimation(path) && !state_.pending_prefix_shrink) {
        file_load_service_.StartLoading(path);
        SetTimer(hwnd_, app_timer::LOADING_ANIM, app_timer::FRAME_INTERVAL_MS, nullptr);
        Invalidate();
        UpdateWindow(hwnd_);
    }
    file_load_service_.StartAsyncLoad(scheduler_, hwnd_, app_msg::PARSE_COMPLETE, renderer_.GetTheme());
}

void App::LoadMarkdownFile(std::wstring_view path)
{
    KillTimer(hwnd_, app_timer::FILE_RELOAD_DEBOUNCE);
    state_.pending_prefix_shrink = false;
    // 仮想パスは NeedsAsyncLoad が true を返し非同期ロードが失敗するため、
    // 先に検出して同期ロードに回す。
    if (IsHelpPath(path)) {
        LoadHelpDocument();
        return;
    }
    const std::pmr::wstring path_str{ path };
    if (!DocumentService::NeedsAsyncLoad(path_str)) {
        file_load_service_.SetLoadingPath(path_str);
        DoLoadMarkdownFile();
    }
    else {
        BeginAsyncLoad(path_str);
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
        auto load_result = file_load_service_.ExecuteLoad(state_.doc, state_.layout_cache);
        if (!load_result) {
            ShowToast(FileLoadErrorMessage(load_result.error(), i18n::S()));
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
        MENDO_TRACE("OnParseComplete: no result (cancelled or load failed)");
        // LoadFile失敗時、FileWatcherがpausedのまま残るのを防ぐ
        doc_service_.ResumeWatching();
        Invalidate();
        return;
    }

    // 差分ベースのスキップ／スクロール復元は同一パスのリロード時のみ有効。
    // 非同期のファイルオープンでも OnParseComplete() が使われるため、
    // 別ファイル読み込み時は差分ロジックをスキップする。
    if (_wcsicmp(result->doc.GetFilePath().c_str(), state_.doc.GetFilePath().c_str()) == 0) {
        const std::string_view old_view(state_.doc.GetRawUtf8());
        const std::string_view new_view(result->doc.GetRawUtf8());
        const size_t diff_pos = FindFirstDifference(old_view, new_view);

        MENDO_TRACEF("OnParseComplete: reload node_count=%zu diff_pos=%zu old_size=%zu new_size=%zu",
            result->doc.GetNodes().size(), diff_pos, old_view.size(), new_view.size());

        if (diff_pos == std::string_view::npos) {
            // 差分なし → リロード不要、監視再開
            state_.pending_prefix_shrink = false;
            doc_service_.ResumeWatching();
            Invalidate();
            return;
        }

        // diff_pos が短い方の末尾と一致 = 片方がもう片方のprefixで、
        // ファイルの伸縮に過ぎない場合はスクロール位置を維持する
        const bool is_prefix_only = IsPrefixOnlyDiff(diff_pos, old_view.size(), new_view.size());

        // エディタの truncate→rewrite 2段階保存の前半（ファイル縮小）を検出。
        // state_.doc を更新せず元のコンテンツを保持することで、次のリロードで
        // 「元コンテンツ vs 最終コンテンツ」の正確な差分を検出できるようにする。
        if (ShouldDeferForTruncateRewrite(is_prefix_only, old_view.size(), new_view.size())) {
            return;
        }

        if (is_prefix_only) {
            // prefix-only はファイル末尾の伸縮のみ。FinishReload が
            // ResizePreservingPrefix で旧キャッシュの実測高さを保持する。
            resource_manager_.CancelMermaidBatch();
            state_.doc = std::move(result->doc);
            FinishReload(true, diff_pos, state_.reload_old_scroll);
            return;
        }

        state_.reload_diff_pos = diff_pos;
    }
    else {
        // 別ファイルの非同期ロードではリロード用の差分スクロールを使わない
        state_.reload_diff_pos = std::string_view::npos;
    }

    state_.doc = std::move(result->doc);
    state_.layout_cache = std::move(result->cache);

    FinishLoadMarkdownFile(/* heights_estimated = */ true);
}

void App::FinishLoadMarkdownFile(bool heights_estimated)
{
    state_.viewport.ClearSelection();
    state_.search_bar_ctrl.Reset();
    PostMessage(hwnd_, app_msg::SEARCH_UNFOCUS, app_param::SEARCH_UNFOCUS_FILE_SWITCH, 0);
    state_.active_toc_index = -1;
    CancelPendingResources();
    renderer_.ShrinkBuffers();

    const std::pmr::wstring dir = state_.doc.GetDirectory();
    if (!dir.empty()) {
        state_.file_explorer.SetDirectory(dir);
        state_.file_explorer.SetCurrentFile(state_.doc.GetFilePath());
    }

    state_.panes.ResetScrollStates();
    renderer_.InvalidateFilePaneCache();
    renderer_.InvalidateTocPaneCache();

    // ビューポート優先レイアウト: 可視範囲のみ計測し、残りは遅延処理に委ねる。
    const auto pane_layout = GetPaneLayout();
    const float md_width = pane_layout.md_rect.width;
    const float md_height = pane_layout.md_rect.height;

    // スクロール位置の復元
    float scroll_y = 0.0f;

    const bool has_reload_diff = (state_.reload_diff_pos != std::string_view::npos);

    MENDO_TRACEF("FinishLoad: has_reload_diff=%d HasNodeRestore=%d HasNavScroll=%d nav_scroll_y=%.1f heights_estimated=%d",
        has_reload_diff ? 1 : 0,
        state_.scroll_restore.HasNodeRestore() ? 1 : 0,
        state_.scroll_restore.HasNavScroll() ? 1 : 0,
        state_.scroll_restore.HasNavScroll() ? state_.scroll_restore.pending_nav_scroll_y : -1.0f,
        heights_estimated ? 1 : 0);

    // cache.Reset()直後は全ノードの高さが0のため、スクロール復元前に
    // ノード高さを推定し、Mermaidキャッシュの実測値で補正する
    if (has_reload_diff
        || state_.scroll_restore.HasNodeRestore()
        || (state_.scroll_restore.HasNavScroll() && state_.scroll_restore.pending_nav_scroll_y > 0.0f)) {
        if (!heights_estimated) {
            MENDO_PROFILE("EstimateNodeHeights");
            EstimateNodeHeights(state_.doc.GetNodes(), state_.layout_cache, renderer_.GetTheme());
        }
        ApplyMermaidCacheHeights(md_width);
    }

    if (has_reload_diff) {
        scroll_y = CalcScrollForDiff(state_.reload_diff_pos, md_height, state_.reload_old_scroll);
        state_.reload_diff_pos = std::string_view::npos;
    }
    else if (state_.scroll_restore.HasNodeRestore()) {
        const int node = std::min(state_.scroll_restore.pending_restore_node,
            static_cast<int>(state_.layout_cache.size()) - 1);
        if (node >= 0) {
            scroll_y = std::max(0.0f,
                state_.layout_cache[node].y_position + static_cast<float>(state_.scroll_restore.pending_restore_offset));
        }
        state_.scroll_restore.ClearNodeRestore();
    }
    else if (state_.scroll_restore.HasNavScroll()) {
        scroll_y = state_.scroll_restore.ConsumeNavScroll();
        // 遅延レイアウトのドリフト補正用（セッション復元と同じ仕組み）
        state_.scroll_restore.pending_restore_scroll_y = scroll_y;
    }
    else {
        // 新規ファイルオープン: 前回ナビゲーションの残留値をクリア
        state_.scroll_restore.pending_restore_scroll_y = -1;
    }

    MENDO_TRACEF("FinishLoad: scroll_y=%.1f (0=top of file)", scroll_y);

    state_.viewport.SetScrollY(scroll_y);

    // 推定→計測の高さ差をアンカー補償
    const bool need_anchor = (scroll_y > 0.0f);
    const auto anchor = need_anchor ? SaveAnchor() : AnchorState{};

    {
        MENDO_PROFILE("ViewportLayout(Initial)");
        layout_service_->ViewportLayout(state_.doc, state_.layout_cache, md_width, md_height);
    }

    if (need_anchor) {
        state_.viewport.AnchorCompensateScroll(anchor.idx, anchor.y_before, state_.layout_cache);
    }

    FinalizeLayout(md_height);

    UpdateTitleBar();

    doc_service_.StartWatching(state_.doc.GetFilePath(), [this]() {
        KillTimer(hwnd_, app_timer::FILE_RELOAD_DEBOUNCE);
        SetTimer(hwnd_, app_timer::FILE_RELOAD_DEBOUNCE, app_timer::FILE_RELOAD_DEBOUNCE_MS, nullptr);
    });
}

void App::ReloadCurrentFile()
{
    const auto& path = state_.doc.GetFilePath();
    if (path.empty() || IsHelpPath(path)) {
        return;
    }
    // ローディングアニメーション表示中は重複リロードを抑制
    if (file_load_service_.IsLoading()) {
        return;
    }

    if (DocumentService::NeedsAsyncLoad(path)) {
        MENDO_TRACE("ReloadCurrentFile: async path");
        state_.reload_old_scroll = state_.viewport.GetScrollY();
        BeginAsyncLoad(path);
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
    state_.active_toc_index = -1;

    if (state_.doc.GetFilePath().empty()) {
        return;
    }

    const float old_scroll = state_.viewport.GetScrollY();

    CancelPendingResources();

    // ファイルを読み込み、旧コンテンツとの差分位置をコピーなしで計算
    auto load_result = [this]() {
        MENDO_PROFILE("Reload::LoadFile");
        return FileLoader::LoadFile(state_.doc.GetFilePath());
    }();
    if (!load_result) {
        doc_service_.ResumeWatching();
        return;
    }
    std::pmr::string new_utf8 = std::move(*load_result);
    const std::string_view old_view(state_.doc.GetRawUtf8());
    const std::string_view new_view(new_utf8);
    const size_t diff_pos = FindFirstDifference(old_view, new_view);

    MENDO_TRACEF("DoReload: diff_pos=%zu old_size=%zu new_size=%zu old_scroll=%.1f",
        diff_pos, old_view.size(), new_view.size(), old_scroll);

    // 差分がなければリロード不要。エディタの保存操作が複数の通知を
    // 発生させた場合に、レイアウトキャッシュの不要なリセットを防ぐ。
    if (diff_pos == std::string_view::npos) {
        state_.pending_prefix_shrink = false;
        doc_service_.ResumeWatching();
        return;
    }

    // 変更箇所のスクロール位置を決定
    // diff_pos が短い方の末尾と一致 = 片方がもう片方のprefixで、
    // ファイルの伸縮に過ぎない場合はスクロール位置を維持する
    const bool is_prefix_only = IsPrefixOnlyDiff(diff_pos, old_view.size(), new_view.size());

    // エディタの truncate→rewrite 2段階保存の前半（ファイル縮小）を検出。
    // state_.doc を更新せず元のコンテンツを保持し、次のリロードで正確な差分を検出する。
    if (ShouldDeferForTruncateRewrite(is_prefix_only, old_view.size(), new_view.size())) {
        return;
    }

    // ドキュメントを新コンテンツで更新
    {
        MENDO_PROFILE("Reload::ReplaceFromMarkdown");
        state_.doc.ReplaceFromMarkdown(std::move(new_utf8));
    }

    FinishReload(is_prefix_only, diff_pos, old_scroll);
}

// DoReloadCurrentFile / OnParseComplete 共通のリロード後処理。
// ドキュメントは更新済みの状態で呼ばれる。
// is_prefix_only: ファイル末尾の伸縮のみか
// diff_pos: 差分開始位置 (prefix-only の場合は使用しない)
// old_scroll: リロード前のスクロール位置
void App::FinishReload(bool is_prefix_only, size_t diff_pos, float old_scroll)
{
    if (is_prefix_only) {
        state_.layout_cache.ResizePreservingPrefix(state_.doc.GetNodes().size());
    }
    else {
        state_.layout_cache.Reset(state_.doc.GetNodes().size(), false);
        EstimateNodeHeights(state_.doc.GetNodes(), state_.layout_cache, renderer_.GetTheme());
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

    MENDO_TRACEF("FinishReload: desired_scroll=%.1f old_scroll=%.1f is_prefix_only=%d",
        desired_scroll, old_scroll, is_prefix_only ? 1 : 0);

    // スクロール位置を設定してからViewportLayoutを呼ぶことで、
    // 変更箇所周辺の可視ノードが優先的に計測される
    state_.viewport.SetScrollY(desired_scroll);

    {
        MENDO_PROFILE("Reload::ViewportLayout");
        layout_service_->ViewportLayout(state_.doc, state_.layout_cache, md_width, md_height);
    }

    FinalizeLayout(md_height);

    if (state_.search_state.IsVisible() && !state_.search_state.GetQuery().empty()) {
        state_.search_bar_ctrl.RunSearchAndLocate(state_.doc.GetNodes());
    }

    doc_service_.ResumeWatching();
}

bool App::ShouldDeferForTruncateRewrite(bool is_prefix_only, size_t old_size, size_t new_size)
{
    if (is_prefix_only && new_size < old_size) {
        // prefix-only shrink が連続する場合もすべて defer する。
        // エディタによっては truncate が複数回発生してから rewrite されることがある。
        state_.pending_prefix_shrink = true;
        doc_service_.ResumeWatching();
        return true;
    }
    state_.pending_prefix_shrink = false;
    return false;
}

float App::CalcScrollForDiff(size_t diff_pos, float viewport_height, float fallback_scroll) const
{
    MENDO_TRACEF("CalcScrollForDiff: diff_pos=%zu node_count=%zu", diff_pos, state_.doc.GetNodes().size());
    return CalcScrollYForDiff(
        state_.doc.GetNodes(), state_.layout_cache,
        std::string_view(state_.doc.GetRawUtf8()),
        diff_pos, viewport_height, fallback_scroll);
}

void App::ApplyMermaidCacheHeights(float md_width)
{
    const float content_width = renderer_.GetTheme().ContentWidth(md_width);
    const bool dark_mode = theme_service_.IsDarkMode();
    const auto& nodes = state_.doc.GetNodes();
    bool any_applied = false;
    for (size_t i : state_.doc.GetMermaidNodeIndices()) {
        const auto hash = mermaid_util::HashCode(nodes[i].text_utf8, content_width, dark_mode);
        MermaidFileCache::CacheEntry fentry;
        if (file_cache_.LookupDimensions(hash, fentry)) {
            state_.layout_cache[i].height = fentry.css_height;
            any_applied = true;
        }
    }
    if (any_applied) {
        RecomputeYPositions(state_.doc.GetNodesMut(), state_.layout_cache, renderer_.GetTheme());
    }
}

void App::UpdateTitleBar()
{
    const int zoom_percent = static_cast<int>(ZOOM_STEPS[state_.viewport.GetZoomIndex()] * 100.0f + 0.5f);
    auto title = BuildTitleString(state_.doc.GetFilePath(), zoom_percent);
    SetWindowTextW(hwnd_, title.c_str());
    state_.cached_title_text = std::move(title);
    InvalidateTitleBar();
}

// OnAppImageLoaded / OnAppReloadFile はWM_APPメッセージ経由でresource_manager_に委譲
void App::OnAppImageLoaded()
{
    Dispatch(ImageLoadedAction{});
}

// ============================================================
// マウスホイール / キーボード
// ============================================================

void App::OnMouseWheel(int px, int py, short delta, bool ctrl)
{
    if (!renderer_.GetRenderTarget()) {
        return;
    }

    if (ctrl) {
        const MouseWheelEvent event{ delta, true, PaneZone::MdPane };
        Dispatch(controller_.HandleMouseWheel(event));
        return;
    }

    // 軸ロック用: 縦スクロール発生をスワイプ検出器に通知
    const bool had_overlay = state_.swipe_detector.IsOverlayVisible();
    state_.swipe_detector.NotifyVScroll(GetTickCount64());
    if (had_overlay) {
        KillTimer(hwnd_, app_timer::SWIPE_OVERLAY);
        Invalidate();
    }

    const auto dip = PixelToDip(px, py);
    const auto pane_layout = GetPaneLayout();
    const auto zone = DetectPaneZone(dip.x, pane_layout,
        renderer_.GetTheme().splitter_width,
        state_.panes.IsFilePaneVisible(), state_.panes.IsTocPaneVisible());

    const MouseWheelEvent event{ delta, false, zone };
    Dispatch(controller_.HandleMouseWheel(event));
}

void App::OnMouseHWheel(short delta)
{
    Dispatch(HWheelAction{ delta, GetTickCount64() });
}

void App::OnKeyDown(WPARAM key)
{
    const KeyDownEvent event{
        static_cast<int>(key),
        (GetKeyState(VK_CONTROL) & 0x8000) != 0,
        (GetKeyState(VK_SHIFT) & 0x8000) != 0,
        (GetKeyState(VK_MENU) & 0x8000) != 0
    };
    Dispatch(controller_.HandleKeyDown(event));
}

void App::Dispatch(const AppAction& action)
{
    // Reducer が cached_pane_layout を参照するため、最新レイアウトを保証する。
    // キャッシュ済みなら O(1) で返る。
    GetPaneLayout();

    auto effects = Reduce(state_, action);
    effect_executor_.Execute(effects);
}

void App::OnDropFiles(HDROP hDrop)
{
    const UINT required = DragQueryFileW(hDrop, 0, nullptr, 0);
    if (required > 0) {
        std::pmr::wstring path(required, L'\0');
        if (DragQueryFileW(hDrop, 0, path.data(), required + 1)) {
            Dispatch(DropFilesAction{ std::move(path) });
        }
    }
    DragFinish(hDrop);
}

void App::OnFileWatchEvent()
{
    Dispatch(FileWatchAction{});
}

void App::HandleTimer(UINT_PTR timer_id)
{
    Dispatch(TimerAction{ timer_id });
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
    Dispatch(CaptureChangedAction{});
}

void App::ShowToast(std::wstring_view message)
{
    state_.toast.Show(message);
    SetTimer(hwnd_, app_timer::TOAST, app_timer::FRAME_INTERVAL_MS, nullptr);
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
    if (!state_.search_state.IsVisible()) {
        return { 0, 0, 1, 1 };
    }
    const auto& layout = GetPaneLayout();
    const auto& r = layout.md_rect;
    const auto sbl = ComputeSearchBarLayout(r.x, r.width, r.y + r.height, !state_.search_state.GetQuery().empty());
    const float s = state_.cached_dpi_scale;
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
    if (!IsHelpPath(state_.doc.GetFilePath())) {
        session_.SaveLastFilePath(state_.doc.GetFilePath());
    }
}

std::pmr::wstring App::LoadLastFilePath() const
{
    return session_.LoadLastFilePath();
}

void App::ShowDirectory(std::wstring_view dir_path)
{
    state_.file_explorer.SetDirectory(dir_path);
    renderer_.InvalidateFilePaneCache();
    Invalidate();
}

// ============================================================
// ペイン状態の永続化
// ============================================================

void App::SavePaneState()
{
    session_.SavePaneState(state_.panes);
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
    session_.LoadPaneState(state_.panes, client_width);
}

void App::SaveScrollPosition()
{
    const int node = FindFirstVisibleNode();
    if (node < 0) {
        return;
    }
    session_.SaveScrollPosition(node, state_.viewport.GetScrollY(), state_.layout_cache[node].y_position);
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
            layout_service_->RecomputeAfterDiagram(state_.doc, state_.layout_cache, renderer_.GetTheme());
        },
        .recompute_layout_anchored = [this]() {
            const auto anchor = SaveAnchor();
            layout_service_->RecomputeAfterDiagram(state_.doc, state_.layout_cache, renderer_.GetTheme());
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
