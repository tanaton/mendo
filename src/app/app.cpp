#include "app.h"
#include "app_constants.h"
#include "app_controller.h"
#include "ascii_util.h"
#include "darkmode_util.h"
#include "document_service.h"
#include "document_utils.h"
#include "gesture_overlay.h"
#include "i18n.h"
#include "layout_queries.h"
#include "pane_layout.h"
#include "profiler.h"
#include "reducer.h"
#include "resource.h"
#include "search_bar_controller.h"
#include "ui_constants.h"
#include "win32_util.h"
#include <thread>
#include <utility>

#pragma comment(lib, "shell32.lib")

namespace {

bool IsEditableTextFile(std::wstring_view path)
{
    return IsMarkdownFile(path) || ascii_util::iequal(ExtensionView(path), L".txt");
}

} // namespace

bool App::Init(HWND hwnd)
{
    hwnd_ = hwnd;

    // 書式とブラシを 1 回で作るため、保存済みテーマ/ズームを Renderer 初期化前に読む。
    theme_service_.LoadDarkMode();
    state_.view.viewport.SetZoomIndex(theme_service_.LoadZoomIndex());
    if (!renderer_.Init(hwnd_, theme_service_.CreateTheme(state_.view.viewport.GetZoomIndex()))) {
        return false;
    }

    layout_service_.emplace(renderer_.GetLayout(), state_.view.viewport);

    // Mermaid 共有 scheduler_ と詰まり合わないよう独立して立ち上げる。
    const auto cores = std::thread::hardware_concurrency();
    layout_scheduler_.Init(LayoutWorkerCount(cores));
    renderer_.SetLayoutScheduler(&layout_scheduler_);

    // PixelToDip 用に DPI スケールをキャッシュ（OnDpiChanged でも更新する）。
    state_.window.cached_dpi_scale = DpiScaleFrom(static_cast<float>(GetDpiForWindow(hwnd_)));

    scheduler_.Init(SharedWorkerCount(cores));

    file_cache_.SetCacheDir(config_.GetConfigPath(L"MermaidCache"));
    file_cache_.Init(MermaidRenderer::EstimateWorkerDpr(), scheduler_);
    mermaid_renderer_.SetFileCache(&file_cache_);

    clipboard_manager_.Init(hwnd_, &file_cache_, &mermaid_renderer_, renderer_.GetWICFactory(), [this](std::wstring_view m) { ShowToast(m); });

    resource_manager_.Init(
        ResourceManagerDeps{
            .doc = &state_.document.doc,
            .cache = &state_.document.layout_cache,
            .viewport = &state_.view.viewport,
            .image_loader = &image_loader_,
            .mermaid = &mermaid_renderer_,
            .mermaid_file_cache = &file_cache_,
            .theme_service = &theme_service_,
        },
        AppResourceManagerCallbacks{ this });
    win32_host_.Init(hwnd_);
    effect_executor_.Init(
        SideEffectExecutorDeps{
            .host = &win32_host_,
            .file_watcher = &file_watcher_,
            .state = &state_,
        },
        AppSideEffectCallbacks{ this });

    mermaid_renderer_.Init(hwnd_, renderer_.GetRenderTarget(), renderer_.GetWICFactory(), scheduler_, app_msg::MERMAID_DISK_LOADED,
                           config_.GetConfigPath(L"WebView2Data"), [this]() {
        resource_manager_.ScheduleMermaidBatch();
    });

    if (!image_loader_.Init(renderer_.GetRenderTarget(), renderer_.GetWICFactory())) {
        OutputDebugStringW(L"[mendo] ImageLoader::Init failed (WIC factory unavailable). Image rendering disabled.\n");
    }
    image_loader_.InitAsync(hwnd_, app_msg::IMAGE_LOADED, scheduler_);

    // D2D デバイスロストでレンダーターゲットが再作成されたら、各ローダーへ伝搬する。
    renderer_.SetDeviceLostCallback([this](ID2D1RenderTarget* new_rt) {
        mermaid_renderer_.SetRenderTarget(new_rt);
        image_loader_.CancelPending();
        image_loader_.SetRenderTarget(new_rt);
        image_loader_.ClearCache();
        // IDWriteTextLayout が SetDrawingEffect 経由で AddRef した旧 RT 由来のブラシと、
        // DiagramEntry::bitmap が新 RT で描画拒否されるのを防ぐ。ApplyCachedImages は
        // bitmap 残存ノードをスキップするため、LoadImages の前に破棄する必要がある。
        state_.document.layout_cache.InvalidateEffects();
        state_.document.layout_cache.InvalidateAllDiagramBitmaps(state_.document.doc.GetNodes());
        resource_manager_.LoadImages();
    });

    RestoreThemeAndZoom();

    cursors_.Init();

    {
        const auto* rt = renderer_.GetRenderTarget();
        state_.window.titlebar.UpdateLayout(rt ? rt->GetSize().width : FALLBACK_WINDOW_WIDTH);
    }

    RestorePaneState();

    state_.ctx_menu.Init(renderer_.GetD2DFactory(), renderer_.GetDWriteFactory());

    state_.interaction.tooltip.Init(hwnd_);
    if (theme_service_.IsDarkMode()) {
        state_.interaction.tooltip.ApplyDarkMode(true);
    }

    state_.search.search_bar_ctrl.Init(state_.search.search_state, state_.view.viewport, state_.document.layout_cache, AppSearchBarCallbacks{ this });
    return true;
}

void App::RestoreThemeAndZoom()
{
    const auto& viewport = state_.view.viewport;
    if (viewport.GetZoomIndex() != ZOOM_DEFAULT_INDEX) {
        state_.view.panes.ApplyZoom(viewport.GetCurrentZoom());
    }
    state_.theme = &renderer_.GetTheme();
    if (theme_service_.IsDarkMode()) {
        ApplyDarkModeToWindow(hwnd_, true);
    }
}

void App::RestorePaneState()
{
    const auto s = session_.LoadPaneState(PaneController::PANE_MIN_WIDTH, PaneController::PANE_DEFAULT_WIDTH);
    auto& panes = state_.view.panes;
    panes.SetSidePaneVisible(PaneTarget::File, s.show_file);
    panes.SetSidePaneVisible(PaneTarget::Toc, s.show_toc);
    panes.SetSidePaneWidth(PaneTarget::File, s.file_width);
    panes.SetSidePaneWidth(PaneTarget::Toc, s.toc_width);
}

void App::LoadInitialDocument()
{
    // small file は preload がここまでに完了している場合が多い。直後の UpdateWindow が
    // 同期 WM_PAINT を発行するため、ここで取り込んでおかないと初回フレームが空になる。
    using PreloadAttachResult = FileLoadService::PreloadAttachResult;
    switch (file_load_service_.AttachOrApplyPreload(hwnd_, app_msg::PARSE_COMPLETE)) {
    case PreloadAttachResult::AppliedSync:
        OnParseComplete();
        break;
    case PreloadAttachResult::AttachedAsync:
        if (DocumentService::ShouldShowLoadingAnimation(file_load_service_.GetLoadingPath())) {
            file_load_service_.BeginLoadingAnimation();
            EmitEffect(effect::SetTimer{ app_timer::Id::LOADING_ANIM, app_timer::FRAME_INTERVAL_MS });
            Invalidate();
        }
        break;
    case PreloadAttachResult::None:
        LoadHelpDocument();
        break;
    }
}

DipPoint App::PixelToDip(int px, int py) const noexcept
{
    return ::PixelToDip(px, py, state_.window.cached_dpi_scale);
}

const PaneLayout& App::GetPaneLayout()
{
    if (!state_.pane_layout_cache.IsValid()) {
        auto* rt = renderer_.GetRenderTarget();
        if (!rt) {
            static const PaneLayout empty{};
            return empty;
        }
        const auto size = rt->GetSize();
        const float tb_h = state_.window.titlebar.GetHeight();
        state_.pane_layout_cache.Set(
            size.width,
            state_.view.panes.ComputeLayout(size.width, size.height, renderer_.GetTheme().splitter_width, tb_h));
    }
    return state_.pane_layout_cache.Get();
}

void App::UpdateTitleBar()
{
    const int zoom_percent = static_cast<int>(state_.view.viewport.GetCurrentZoom() * 100.0f + 0.5f);
    auto title = BuildTitleString(state_.document.doc.GetFilePath(), zoom_percent);
    if (title == state_.cached_title_text) {
        return;
    }
    SetWindowTextW(hwnd_, title.c_str());
    state_.cached_title_text = std::move(title);
    Invalidate();
}

PaneZone App::PaneAtPoint(float dip_x)
{
    if (!IsRenderReady()) {
        return PaneZone::None;
    }
    return ZoneAt(dip_x, GetPaneLayout());
}

// ホバー/ホイール/クリック系のゾーン判定を 1 箇所に揃え、食い違わないようにする。
PaneZone App::ZoneAt(float dip_x, const PaneLayout& layout) const noexcept
{
    return DetectPaneZone(
        dip_x,
        layout,
        renderer_.GetTheme().splitter_width,
        state_.view.panes.IsSidePaneVisible(PaneTarget::File),
        state_.view.panes.IsSidePaneVisible(PaneTarget::Toc));
}

SidePaneState App::BuildSidePaneState(const PaneLayout& layout) const
{
    const auto& panes = state_.view.panes;
    // GetEntries は未列挙なら列挙するため、非表示のファイルペインでは呼ばない。
    static const std::pmr::vector<FileEntry> kNoEntries;
    const auto make_side_pane = [this, &panes](PaneTarget t, PaneRect rect) {
        return SidePaneInstance{
            .rect = rect,
            .scroll = panes.SidePaneScroll(t),
            .hovered_index = panes.GetHoveredSideIndex(t),
            .show = panes.IsSidePaneVisible(t),
            .hovered_button = panes.GetSideHoveredButton(t),
            .reveal_enabled = SidePaneHeaderButtonsFor(state_, t).reveal_enabled,
        };
    };
    return SidePaneState{
        .panes = {
                  make_side_pane(PaneTarget::File, layout.file_rect),
                  make_side_pane(PaneTarget::Toc, layout.toc_rect),
                  },
        .file_entries = panes.IsSidePaneVisible(PaneTarget::File) ? state_.file_explorer.GetEntries() : kNoEntries,
        .toc_entries = state_.document.doc.GetToc().GetEntries(),
        .nodes = state_.document.doc.GetNodes(),
        .active_toc_index = state_.view.active_toc_index,
    };
}

TitleBarRenderState App::BuildTitleBarRenderState() const
{
    const auto& tbar = state_.window.titlebar;
    const auto& panes = state_.view.panes;
    return TitleBarRenderState{
        .title_text = state_.cached_title_text,
        .buttons = tbar.GetButtons(),
        .icon_rect = tbar.GetIconRect(),
        .title_text_rect = tbar.GetTitleTextRect(),
        .height = tbar.GetHeight(),
        .window_width = state_.pane_layout_cache.WindowWidth(),
        .hovered_zone = tbar.GetHovered(),
        .is_dark_mode = theme_service_.IsDarkMode(),
        .search_active = state_.search.search_state.IsVisible(),
        .file_pane_visible = panes.IsSidePaneVisible(PaneTarget::File),
        .toc_pane_visible = panes.IsSidePaneVisible(PaneTarget::Toc),
        .is_maximized = IsZoomed(hwnd_) != FALSE,
        .window_active = state_.window.window_active,
    };
}

void App::SyncRendererSearchMatches()
{
    const auto& ss = state_.search.search_state;
    if (ss.IsVisible() && ss.IsHighlightEnabled() && !ss.GetMatches().empty()) {
        renderer_.SetSearchMatches(&ss.GetMatches(), ss.GetCurrentMatchIndex(), ss.GetGeneration());
    }
    else {
        renderer_.SetSearchMatches(nullptr, -1, 0);
    }
}

void App::OnPaint()
{
    MENDO_PROFILE("OnPaint");

    PAINTSTRUCT ps;
    BeginPaint(hwnd_, &ps);

    const auto& layout = GetPaneLayout();
    const bool show_loading = file_load_service_.IsLoading();
    if (!show_loading) {
        const bool updated = layout_service_->EnsureVisibleLayout(state_.document.doc, state_.document.layout_cache, layout.md_rect.width, layout.md_rect.height);
        if (updated) {
            SyncMaxScroll(layout.md_rect.height);
            // 未計測領域に入った。スクロール先を先読み計測させ、以降のフレームでの同期計測を減らす。
            ScheduleDeferredLayoutIfNeeded();
        }
    }

    const auto gs = ResolveGestureOverlay(state_.interaction.gesture, state_.interaction.swipe_detector);
    const SidePaneState sp = BuildSidePaneState(layout);
    const TitleBarRenderState tb = BuildTitleBarRenderState();
    const ToastRenderState ts{ .message = state_.interaction.toast.GetMessage() };

    if (show_loading) {
        renderer_.DrawLoading(file_load_service_.GetLoadingAngle(), layout.md_rect, sp, tb, gs, ts);
    }
    else {
        SyncRendererSearchMatches();

        // 描画コマンドが各ノードのエフェクト状態を参照するため Render より前に行う。
        renderer_.PrepareVisibleEffects(
            state_.document.doc.GetNodes(), state_.document.layout_cache,
            state_.view.viewport.GetScrollY(), layout.md_rect.height);

        const auto sb = state_.search.search_bar_ctrl.BuildRenderState();
        const BlockHScrollContext h_scroll{
            .scroll_x = &state_.view.block_scroll_x,
            .hovered_block = state_.view.hovered_h_block,
            .drag_block = state_.view.h_drag_node,
        };
        renderer_.Render(
            { state_.document.doc.GetNodes(), state_.document.layout_cache,
              state_.view.viewport.GetSelection(), layout.md_rect, sp, tb, gs, ts, sb,
              state_.view.viewport.GetScrollY(), ScrollableContentHeight(),
              state_.interaction.nav_hover, state_.interaction.hovered,
              state_.view.nav_history.CanGoBack(), state_.view.nav_history.CanGoForward(),
              layout_service_->HasDirtyNodes(), h_scroll });
    }

    EndPaint(hwnd_, &ps);
    MENDO_FRAME_MARK();
}

void App::OnResize(UINT width, UINT height)
{
    Dispatch(ResizeAction{ width, height });
}

void App::OnMinimize()
{
    renderer_.TrimForMinimize();
}

void App::OnDpiChanged(UINT dpi, const RECT* suggested)
{
    // reducer をプラットフォーム非依存に保つため、Win32 の RECT を PixelRect に詰め替える。
    const PixelRect rc{
        static_cast<int32_t>(suggested->left),
        static_cast<int32_t>(suggested->top),
        static_cast<int32_t>(suggested->right),
        static_cast<int32_t>(suggested->bottom),
    };
    Dispatch(DpiChangedAction{ static_cast<uint32_t>(dpi), rc });
}

void App::OnAppImageLoaded()
{
    image_loader_.ProcessCompletedDecodes();
}

void App::OnMermaidDiskLoaded()
{
    resource_manager_.BatchMermaidCompletions([this] { mermaid_renderer_.ProcessDiskLoads(); });
}

void App::OnKeyDown(WPARAM key)
{
    const KeyDownEvent event{
        static_cast<int>(key),
        IsKeyDown(VK_CONTROL),
        IsKeyDown(VK_SHIFT),
        IsKeyDown(VK_MENU)
    };
    Dispatch(app_controller::HandleKeyDown(event));
}

void App::Dispatch(const AppAction& action)
{
    // reducer は pane_layout_cache を参照するだけなので、ここで確実に確定させておく。
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
    file_watcher_.CheckForChanges();
}

// AppState を更新するタイマーだけ reducer に渡し、サービスを進めるだけのものはここで直接呼ぶ。
void App::HandleTimer(UINT_PTR timer_id)
{
    if (timer_id < std::to_underlying(app_timer::kFirstTimer) || timer_id > std::to_underlying(app_timer::kLastTimer)) {
        return;
    }
    using enum app_timer::Id;
    const auto id = static_cast<app_timer::Id>(timer_id);
    switch (id) {
    case SWIPE_OVERLAY:
    case TOAST:
    case TOOLTIP:
    case SEARCH_DEBOUNCE:
        Dispatch(TimerAction{ id });
        return;
    case DEFERRED_LAYOUT:
        OnDeferredLayout();
        return;
    case LOADING_ANIM:
        file_load_service_.TickLoadingAnimation();
        Invalidate();
        return;
    case MERMAID_BATCH:
        resource_manager_.ProcessMermaidBatch();
        return;
    case BITMAP_MANAGE:
        resource_manager_.OnBitmapManageTimer();
        return;
    case MERMAID_INIT_RETRY:
        mermaid_renderer_.OnInitRetryTimer();
        return;
    case MERMAID_IDLE:
        mermaid_renderer_.OnIdleTimer();
        return;
    case FILE_RELOAD_DEBOUNCE:
        EmitEffect(effect::KillTimer{ id });
        ReloadCurrentFile();
        return;
    case END:
        break;
    }
    std::unreachable();
}

void App::OnCaptureChanged()
{
    Dispatch(CaptureChangedAction{});
}

void App::ShowToast(std::wstring_view message)
{
    if (message.empty()) {
        return;
    }
    EmitEffect(effect::ShowToast{ std::pmr::wstring{ message } });
}

void App::SaveSession()
{
    // SaveScrollPosition だけガード外に置くと、Help 表示中に終了したとき
    // 前回 LastFilePath に Help の node index が紐付き、次回起動時に誤位置へジャンプする。
    const auto& doc = state_.document.doc;
    if (doc.HasBackingFile()) {
        session_.SaveLastFilePath(doc.GetFilePath());
        const auto& cache = state_.document.layout_cache;
        if (const int node = state_.view.viewport.FindFirstVisibleNode(cache); node >= 0) {
            // 復元側 (NodeOffsetToScrollY) と同じ cache[node].text_top を読む。
            session_.SaveScrollPosition(node, state_.view.viewport.GetScrollY(), cache.Top(static_cast<size_t>(node)));
        }
    }

    const auto& panes = state_.view.panes;
    session_.SavePaneState({
        .show_file = panes.IsSidePaneVisible(PaneTarget::File),
        .show_toc = panes.IsSidePaneVisible(PaneTarget::Toc),
        .file_width = panes.GetSidePaneWidth(PaneTarget::File),
        .toc_width = panes.GetSidePaneWidth(PaneTarget::Toc),
    });

    config_.SaveWString(kConfigSectionGeneral, kConfigKeyLanguage, i18n::GetLangKey());
    // 個別 Save 呼び出しでは write が遅延されるため、終了前に明示 flush で
    // すべての設定値を 1 度のディスク書き込みにまとめる。
    config_.Flush();
}

void App::OnDestroy()
{
    mermaid_renderer_.Shutdown();
    // 走行中タスクが latch.wait 中の参照を保ったまま解放されないよう、
    // LayoutEngine の参照解除 → Shutdown (join) → ターゲット deinit の順を守る。
    renderer_.SetLayoutScheduler(nullptr);
    layout_scheduler_.Shutdown();
    // Shutdown はキューを最後まで処理するので、結果を使わないタスクは先に空振りさせる。
    image_loader_.CancelPending();
    file_load_service_.CancelAsyncLoad();
    scheduler_.Shutdown();
    file_cache_.Shutdown();
    file_cache_.SaveIndex();

    SaveSession();

    for (UINT_PTR id = std::to_underlying(app_timer::kFirstTimer); id <= std::to_underlying(app_timer::kLastTimer); ++id) {
        KillTimer(hwnd_, id);
    }
}

SearchBarLayout App::ComputeSearchBarLayoutForMd(const PaneRect& md_rect) const
{
    return ComputeSearchBarLayout(md_rect, !state_.search.search_state.GetQuery().empty());
}

std::optional<SearchBarLayout> App::SearchBarLayoutAt(float dip_y, const PaneRect& md_rect) const
{
    if (!state_.search.search_state.IsVisible()) {
        return std::nullopt;
    }
    const auto sbl = ComputeSearchBarLayoutForMd(md_rect);
    if (dip_y < sbl.bar_top) {
        return std::nullopt;
    }
    return sbl;
}

int App::HitTestSearchInputPos(const SearchBarLayout& sbl, std::wstring_view query_wide, float dip_x) const
{
    return renderer_.HitTestSearchInput(query_wide, dip_x - sbl.text_left(), sbl.text_width());
}

RECT App::GetSearchEditRect()
{
    if (!state_.search.search_state.IsVisible()) {
        return { 0, 0, 1, 1 };
    }
    const auto sbl = ComputeSearchBarLayoutForMd(GetPaneLayout().md_rect);
    const float s = state_.window.cached_dpi_scale;
    return {
        DipToPixel(sbl.input_rect.left, s),
        DipToPixel(sbl.input_rect.top, s),
        DipToPixel(sbl.input_rect.right, s),
        DipToPixel(sbl.input_rect.bottom, s),
    };
}

void App::SetInitialDirectory(std::wstring_view dir_path)
{
    state_.file_explorer.SetDirectory(dir_path);
}

void App::FinishThemeOrZoomChange()
{
    const PaneRect md = GetPaneLayout().md_rect;

    ViewportLayout(md.width, md.height);
    SyncMaxScroll(md.height);
    resource_manager_.RequestMermaidRenders();
    SettleLayout();
    Invalidate();

    EmitEffect(effect::SyncTocActive{});
}

void App::HandleApplyThemeChange(const effect::ApplyThemeChange& e)
{
    if (e.type == effect::ApplyThemeChange::Type::Zoom) {
        renderer_.ApplyZoomFromBase(theme_service_.CreateTheme(), state_.view.viewport.GetCurrentZoom());
        FinishThemeOrZoomChange();
        UpdateTitleBar();
        theme_service_.SaveZoomLevel(state_.view.viewport.GetZoomIndex());
        return;
    }

    theme_service_.ToggleDarkMode();
    renderer_.SetTheme(theme_service_.CreateTheme(state_.view.viewport.GetZoomIndex()));
    const bool dark = theme_service_.IsDarkMode();
    ApplyDarkModeToWindow(hwnd_, dark);
    state_.interaction.tooltip.ApplyDarkMode(dark);
    // メモリキャッシュはキーにダークモードを含むので消さず、往復時に再利用する。
    mermaid_renderer_.CancelPending();
    FinishThemeOrZoomChange();
    theme_service_.SaveDarkMode();
}

void App::OnContextMenu(int screen_x, int screen_y)
{
    POINT client_pt{ screen_x, screen_y };
    ScreenToClient(hwnd_, &client_pt);
    const auto dip = PixelToDip(client_pt.x, client_pt.y);

    const auto& selection = state_.view.viewport.GetSelection();
    const auto& panes = state_.view.panes;
    const ContextMenuParams params{
        .screen_x = screen_x,
        .screen_y = screen_y,
        .dpi_scale = state_.window.cached_dpi_scale,
        .can_go_back = state_.view.nav_history.CanGoBack(),
        .can_go_forward = state_.view.nav_history.CanGoForward(),
        // ヘルプは埋め込みリソースで、エディタで開くファイルが無い。
        .has_file = state_.document.doc.HasBackingFile(),
        .has_selection = selection.active && selection.start_node >= 0,
        .dark_mode_checked = theme_service_.IsDarkMode(),
        .file_pane_checked = panes.IsSidePaneVisible(PaneTarget::File),
        .toc_pane_checked = panes.IsSidePaneVisible(PaneTarget::Toc),
        .show_file_items = PaneAtPoint(dip.x) == PaneZone::MdPane,
        .theme = &renderer_.GetTheme(),
    };

    switch (state_.ctx_menu.Show(hwnd_, params)) {
    case IDM_NAV_BACK:
        Dispatch(NavigateBackAction{});
        break;
    case IDM_NAV_FORWARD:
        Dispatch(NavigateForwardAction{});
        break;
    case IDM_EDIT_FILE: {
        const auto& file_path = state_.document.doc.GetFilePath();
        if (IsEditableTextFile(file_path)) {
            win32_host_.ShellOpen(file_path);
        }
        break;
    }
    case IDM_COPY:
        Dispatch(CopyClipboardAction{});
        break;
    case IDM_COPY_FORMATTED:
        Dispatch(CopyFormattedClipboardAction{});
        break;
    case IDM_TOGGLE_DARK_MODE:
        Dispatch(ToggleDarkModeAction{});
        break;
    case IDM_TOGGLE_FILE_PANE:
        Dispatch(TogglePaneAction{ PaneTarget::File });
        break;
    case IDM_TOGGLE_TOC_PANE:
        Dispatch(TogglePaneAction{ PaneTarget::Toc });
        break;
    default:
        break;
    }
}

void App::EnsureScrollTarget()
{
    state_.view.viewport.EnsureScrollTarget(state_.document.layout_cache);
}

void App::ViewportLayout(float md_width, float md_height)
{
    layout_service_->ViewportLayout(state_.document.doc, state_.document.layout_cache, md_width, md_height);
}

void App::SyncMaxScroll(float md_height)
{
    state_.view.viewport.SyncMaxScroll(ScrollableContentHeight(), md_height);
}

float App::ScrollableContentHeight() const noexcept
{
    // WM_NCHITTEST は Init 前にも届く。
    return state_.theme ? MdScrollableContentHeight(state_) : 0.0f;
}

// Mermaid のキャッシュキーに入るため、全経路でこの値を使う (ずれるとキャッシュを外す)。
float App::MdContentWidth()
{
    return renderer_.GetTheme().ContentWidth(GetPaneLayout().md_rect.width);
}

bool App::ScheduleDeferredLayoutIfNeeded()
{
    if (!layout_service_->HasDirtyNodes()) {
        return false;
    }
    EmitEffect(effect::SetTimer{ app_timer::Id::DEFERRED_LAYOUT, app_timer::FRAME_INTERVAL_MS });
    return true;
}

// ロード・ズーム・リサイズでは画面外にもレイアウトが作られ (初回の MeasureTable は全セル)、
// 本文幅が広がると画像の解像度が足りなくなる。スクロールを待たずに BitmapManage で整理させる。
// 遅延レイアウトが残っていれば、その完了時 (OnDeferredLayout) に出す。
void App::SettleLayout()
{
    if (!ScheduleDeferredLayoutIfNeeded()) {
        EmitEffect(effect::BitmapManage{});
    }
}

void App::FinalizeLayout(float md_pane_height)
{
    resource_manager_.LoadImages();
    resource_manager_.RequestMermaidRenders();
    SyncMaxScroll(md_pane_height);
    Invalidate();
    SettleLayout();
}

void App::InvalidateHitPositions()
{
    Dispatch(InvalidateHoverAction{});
}

void App::SyncTocActiveAndAutoScroll(bool auto_scroll)
{
    if (!state_.view.panes.IsSidePaneVisible(PaneTarget::Toc)) {
        return;
    }
    const auto& theme = renderer_.GetTheme();
    const int new_active = FindActiveTocIndex(
        state_.document.doc.GetToc(), state_.document.layout_cache, state_.view.viewport.GetScrollY(), theme.heading_spacing_above);
    if (new_active == state_.view.active_toc_index) {
        return;
    }
    state_.view.active_toc_index = new_active;

    // auto_scroll=false (目次クリック由来) ではハイライト更新のみ。
    // ユーザーが操作中の目次ペインのスクロール位置を勝手に動かさない (issue#259)。
    if (!auto_scroll || new_active < 0) {
        return;
    }

    // アクティブ見出しを目次ペインの中央に保つように追従スクロールする。
    const auto ctx = GetSidePaneContext(state_, PaneTarget::Toc);
    if (ctx.info.content_height > 0.0f) {
        ctx.scroll.scroll_y = CenterPaneScrollOnItem(static_cast<size_t>(new_active), theme.pane_item_height, ctx.info);
    }
}

void App::OnResizeEnd()
{
    MENDO_PROFILE("OnResizeEnd");

    EmitEffect(effect::KillTimer{ app_timer::Id::DEFERRED_LAYOUT });

    const PaneRect md = GetPaneLayout().md_rect;

    EnsureScrollTarget();

    {
        MENDO_PROFILE("ViewportLayout(Resize)");
        ViewportLayout(md.width, md.height);
    }
    SyncMaxScroll(md.height);
    Invalidate();

    SettleLayout();

    resource_manager_.ScheduleMermaidBatch();

    EmitEffect(effect::SyncTocActive{});
}

void App::RefreshPaneLayout()
{
    state_.pane_layout_cache.Invalidate();
    EmitEffect(effect::PerformResizeEnd{});
}

void App::OnDeferredLayout()
{
    MENDO_PROFILE("OnDeferredLayout");

    constexpr int kDeferredLayoutBatchNodes = 200;

    const PaneRect md = GetPaneLayout().md_rect;
    bool more;
    {
        MENDO_PROFILE("ProcessDirtyBatch");
        more = layout_service_->ProcessDirtyBatch(
            state_.document.doc, state_.document.layout_cache, md.width, kDeferredLayoutBatchNodes,
            LayoutService::ViewportLimit{ md.height, ResourceManager::EVICT_BUFFER_SCREENS });
    }

    // 中間バッチでは SyncMaxScroll のクランプを遅延させる。
    // ビューポート後のノードが計測されると total_height が縮小し、中間的な
    // max_scroll に基づくクランプで scroll_y が不当に引き下げられるのを防ぐ。
    // スクロールバートラッキング中はユーザー操作を優先してクランプを反映する。
    if (state_.view.panes.GetDragTarget() == PaneController::DragTarget::MdScrollbar) {
        SyncMaxScroll(md.height);
    }

    if (more) {
        return;
    }
    EmitEffect(effect::KillTimer{ app_timer::Id::DEFERRED_LAYOUT });

    // 同期ディスク I/O + PNG デコードで UI スレッドを長時間ブロックしないよう、
    // Mermaid ファイルキャッシュの読み込みは時間予算付きバッチに回す。
    resource_manager_.ScheduleMermaidBatch();
    EmitEffect(effect::BitmapManage{});

    SyncMaxScroll(md.height);
    Invalidate();

    // layout_cache の text_top が確定したので目次アクティブ見出しを再同期する。
    EmitEffect(effect::SyncTocActive{});
}
