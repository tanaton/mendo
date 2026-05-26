#include "app.h"
#include "app_constants.h"
#include "darkmode_util.h"
#include "document_service.h"
#include "file_dialog_service.h"
#include "file_loader.h"
#include "i18n.h"
#include "mermaid_util.h"
#include "pane_layout.h"
#include "resource.h"
#include "ui_constants.h"
#include <algorithm>
#include <thread>

bool App::Init(HWND hwnd)
{
    hwnd_ = hwnd;

    if (!renderer_.Init(hwnd_)) {
        return false;
    }

    layout_service_.emplace(renderer_.GetLayout(), state_.view.viewport);

    // Mermaid 共有 scheduler_ と詰まり合わないよう独立して立ち上げる。
    {
        // 上限 16 は超大規模 dirty バッチでも latch 待ちオーバーヘッドが利得を相殺する境界。
        // 下限 2 は hardware_concurrency()==0 や 1 コア環境でも並列計測の枠組みを保つため。
        constexpr unsigned kMinLayoutWorkers = 2u;
        constexpr unsigned kMaxLayoutWorkers = 16u;
        const auto cores = std::thread::hardware_concurrency();
        const int layout_workers = static_cast<int>(std::clamp<unsigned>(
            cores > 0 ? cores - 1 : kMinLayoutWorkers, kMinLayoutWorkers, kMaxLayoutWorkers));
        layout_scheduler_.Init(layout_workers);
        renderer_.GetLayout().SetLayoutScheduler(&layout_scheduler_);
    }

    // PixelToDip 用に DPI スケールをキャッシュ（OnDpiChanged でも更新する）。
    const float init_dpi = static_cast<float>(GetDpiForWindow(hwnd_));
    state_.window.cached_dpi_scale = DpiScaleFrom(init_dpi);

    scheduler_.Init(mermaid_util::ComputeWorkerCount(std::thread::hardware_concurrency()));

    const auto config_dir = config_.GetConfigDir();
    if (!config_dir.empty()) {
        file_cache_.SetCacheDir(config_dir / L"MermaidCache");
    }
    file_cache_.Init(state_.window.cached_dpi_scale, scheduler_);
    mermaid_renderer_.SetFileCache(&file_cache_);

    clipboard_manager_.Init(hwnd_, &file_cache_, &mermaid_renderer_, [this](std::wstring_view m) { ShowToast(m); });

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
    win32_host_.Init(hwnd_, cursors_);
    effect_executor_.Init(
        SideEffectExecutorDeps{
            .host = &win32_host_,
            .file_watcher = &file_watcher_,
            .state = &state_,
            .layout_service = &*layout_service_,
        },
        AppSideEffectCallbacks{ this });

    const auto webview2_data = config_dir.empty() ? std::filesystem::path{} : config_dir / L"WebView2Data";
    mermaid_renderer_.Init(hwnd_, renderer_.GetRenderTarget(), renderer_.GetWICFactory(), webview2_data, [this]() {
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
        resource_manager_.LoadImages();
        // IDWriteTextLayout が SetDrawingEffect 経由で AddRef した旧 RT 由来のブラシと、
        // DiagramEntry::bitmap が新 RT で描画拒否されるのを防ぐ。
        state_.document.layout_cache.InvalidateEffectsAndDiagramBitmaps(state_.document.doc.GetNodes());
    });

    theme_service_.LoadDarkMode();
    state_.view.viewport.SetZoomIndex(theme_service_.LoadZoomIndex());
    if (theme_service_.IsDarkMode() || state_.view.viewport.GetZoomIndex() != ZOOM_DEFAULT_INDEX) {
        renderer_.SetTheme(theme_service_.CreateTheme(state_.view.viewport.GetZoomIndex()));
        if (state_.view.viewport.GetZoomIndex() != ZOOM_DEFAULT_INDEX) {
            state_.view.panes.ApplyZoom(state_.view.viewport.GetCurrentZoom());
        }
    }
    state_.theme = &renderer_.GetTheme();
    if (theme_service_.IsDarkMode()) {
        ApplyDarkModeToWindow(hwnd_, true);
    }

    cursors_.Init();

    {
        const auto* rt = renderer_.GetRenderTarget();
        const float window_w = rt ? rt->GetSize().width : FALLBACK_WINDOW_WIDTH;
        state_.window.titlebar.UpdateLayout(window_w);
    }

    {
        float client_width = 0.0f;
        if (hwnd_) {
            RECT rc{};
            if (GetClientRect(hwnd_, &rc)) {
                client_width = static_cast<float>(rc.right - rc.left);
            }
        }
        const auto s = session_.LoadPaneState(client_width, PaneController::PANE_MIN_WIDTH, PaneController::PANE_DEFAULT_WIDTH);
        auto& panes = state_.view.panes;
        panes.SetSidePaneVisible(PaneTarget::File, s.show_file);
        panes.SetSidePaneVisible(PaneTarget::Toc, s.show_toc);
        panes.SetSidePaneWidth(PaneTarget::File, s.file_width);
        panes.SetSidePaneWidth(PaneTarget::Toc, s.toc_width);
    }

    state_.ctx_menu.Init(renderer_.GetD2DFactory(), renderer_.GetDWriteFactory());

    state_.interaction.tooltip.Init(hwnd_);
    if (theme_service_.IsDarkMode()) {
        state_.interaction.tooltip.ApplyDarkMode(true);
    }

    state_.search.search_bar_ctrl.Init(state_.search.search_state, state_.view.viewport, state_.document.layout_cache, AppSearchBarCallbacks{ this });

    // small file は preload が App::Init より先に完了している場合が多い。直後の
    // ShowWindow/UpdateWindow が同期 WM_PAINT を発行するため、ここで結果を取り込んで
    // おかないと初回フレームが空ウィンドウになってしまう。
    using PreloadAttachResult = FileLoadService::PreloadAttachResult;
    switch (file_load_service_.AttachOrApplyPreload(hwnd_, app_msg::PARSE_COMPLETE)) {
    case PreloadAttachResult::AppliedSync:
        EmitEffect(effect::HandleParseComplete{});
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

    return true;
}
