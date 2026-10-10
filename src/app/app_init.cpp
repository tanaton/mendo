#include "app.h"
#include "app_constants.h"
#include "darkmode_util.h"
#include "document_service.h"
#include "mermaid_util.h"
#include "ui_constants.h"
#include <algorithm>
#include <thread>

namespace {

// 上限 16 は超大規模 dirty バッチでも latch 待ちオーバーヘッドが利得を相殺する境界。
// 下限 2 は hardware_concurrency()==0 や 1 コア環境でも並列計測の枠組みを保つため。
int LayoutWorkerCount(unsigned cores) noexcept
{
    constexpr unsigned kMinLayoutWorkers = 2u;
    constexpr unsigned kMaxLayoutWorkers = 16u;
    return static_cast<int>(std::clamp<unsigned>(
        cores > 0 ? cores - 1 : kMinLayoutWorkers, kMinLayoutWorkers, kMaxLayoutWorkers));
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

    scheduler_.Init(mermaid_util::ComputeWorkerCount(cores));

    file_cache_.SetCacheDir(config_.GetConfigPath(L"MermaidCache"));
    file_cache_.Init(state_.window.cached_dpi_scale, scheduler_);
    mermaid_renderer_.SetFileCache(&file_cache_);
    mermaid_renderer_.SetBackgroundScheduler(&scheduler_, app_msg::MERMAID_DISK_LOADED);

    clipboard_manager_.Init(hwnd_, &file_cache_, &mermaid_renderer_, renderer_.GetWICFactory(), [this](std::wstring_view m) { ShowToast(m); });

    resource_manager_.Init(
        ResourceManagerDeps{
            .doc = &state_.document.doc,
            .cache = &state_.document.layout_cache,
            .viewport = &state_.view.viewport,
            .image_loader = &image_loader_,
            .mermaid = &mermaid_renderer_,
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

    mermaid_renderer_.Init(hwnd_, renderer_.GetRenderTarget(), renderer_.GetWICFactory(), config_.GetConfigPath(L"WebView2Data"), [this]() {
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
        state_.document.layout_cache.InvalidateAllDiagramBitmaps();
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

    AttachPreload();
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

void App::AttachPreload()
{
    // small file は preload が App::Init より先に完了している場合が多い。直後の
    // ShowWindow/UpdateWindow が同期 WM_PAINT を発行するため、ここで結果を取り込んで
    // おかないと初回フレームが空ウィンドウになってしまう。
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
        break;
    }
}
