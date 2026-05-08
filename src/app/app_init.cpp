#include "app.h"
#include "app_constants.h"
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
        const auto cores = std::thread::hardware_concurrency();
        const int layout_workers = static_cast<int>(std::clamp<unsigned>(cores > 0 ? cores - 1 : 2, 2u, 16u));
        layout_scheduler_.Init(layout_workers);
        renderer_.GetLayout().SetLayoutScheduler(&layout_scheduler_);
    }

    // PixelToDip 用に DPI スケールをキャッシュ（OnDpiChanged でも更新する）。
    const float init_dpi = static_cast<float>(GetDpiForWindow(hwnd_));
    state_.window.cached_dpi_scale = (init_dpi > 0.0f) ? (init_dpi / DEFAULT_DPI) : 1.0f;

    scheduler_.Init(mermaid_util::ComputeWorkerCount(
        std::thread::hardware_concurrency()));

    const auto config_dir = config_.GetConfigDir();
    if (!config_dir.empty()) {
        file_cache_.SetCacheDir(config_dir / L"MermaidCache");
    }
    file_cache_.Init(state_.window.cached_dpi_scale, scheduler_);
    mermaid_renderer_.SetFileCache(&file_cache_);

    resource_manager_.Init(
        state_.document.doc,
        state_.document.layout_cache,
        state_.view.viewport,
        image_loader_,
        mermaid_renderer_,
        theme_service_,
        renderer_.GetTheme(),
        BuildResourceManagerCallbacks());
    win32_host_.Init(hwnd_, cursors_);
    // clang-format off
    effect_executor_.Init(
        win32_host_,
        resource_manager_,
        doc_service_,
        state_,
        *layout_service_,
        {
            .load_file = [this](std::wstring_view path) {
                LoadMarkdownFile(path);
            },
            .reload_file = [this]() {
                ReloadCurrentFile();
            },
            .open_file_dialog = [this]() {
                const auto path = FileLoader::OpenFileDialog(hwnd_);
                if (!path.empty()) {
                    if (!state_.document.doc.GetFilePath().empty()) {
                        PushCurrentNavEntry(state_);
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
                EmitEffect(effect::SyncMaxScroll{ sizing_layout.md_rect.height });
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
            .show_context_menu = [this](int x, int y) {
                OnContextMenu(x, y);
            },
            .sync_toc_active = [this]() {
                SyncTocActiveAndAutoScroll();
            },
        }
    );
    // clang-format on

    const auto webview2_data = config_dir.empty() ? std::filesystem::path{} : config_dir / L"WebView2Data";
    mermaid_renderer_.Init(hwnd_, renderer_.GetRenderTarget(), renderer_.GetWICFactory(), webview2_data, [this]() {
        resource_manager_.ScheduleMermaidBatch();
    });

    image_loader_.Init(renderer_.GetRenderTarget(), renderer_.GetWICFactory());
    image_loader_.InitAsync(hwnd_, app_msg::IMAGE_LOADED, scheduler_);

    // D2D デバイスロストでレンダーターゲットが再作成されたら、各ローダーへ伝搬する。
    renderer_.SetDeviceLostCallback([this](ID2D1RenderTarget* new_rt) {
        mermaid_renderer_.SetRenderTarget(new_rt);
        image_loader_.CancelPending();
        image_loader_.SetRenderTarget(new_rt);
        image_loader_.ClearCache();
        resource_manager_.LoadImages();
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

    SyncPaneThemeCache();

    cursors_.Init();

    {
        const auto* rt = renderer_.GetRenderTarget();
        const float window_w = rt ? rt->GetSize().width : 1600.0f;
        state_.window.titlebar.UpdateLayout(window_w);
    }

    LoadPaneState();

    state_.ctx_menu.Init(renderer_.GetD2DFactory(), renderer_.GetDWriteFactory());

    state_.interaction.tooltip.Init(hwnd_);
    if (theme_service_.IsDarkMode()) {
        state_.interaction.tooltip.ApplyDarkMode(true);
    }

    state_.search.search_bar_ctrl.Init(state_.search.search_state, state_.view.viewport, state_.document.layout_cache, BuildSearchBarCallbacks());

    // small file は preload が App::Init より先に完了している場合が多い。直後の
    // ShowWindow/UpdateWindow が同期 WM_PAINT を発行するため、ここで結果を取り込んで
    // おかないと初回フレームが空ウィンドウになってしまう。
    using PreloadAttachResult = FileLoadService::PreloadAttachResult;
    switch (file_load_service_.AttachOrApplyPreload(hwnd_, app_msg::PARSE_COMPLETE)) {
    case PreloadAttachResult::AppliedSync:
        OnParseComplete();
        break;
    case PreloadAttachResult::AttachedAsync:
        if (DocumentService::NeedsLoadingAnimation(file_load_service_.GetLoadingPath())) {
            file_load_service_.BeginLoadingAnimation();
            EmitEffect(effect::SetTimer{ app_timer::LOADING_ANIM, app_timer::FRAME_INTERVAL_MS });
            Invalidate();
        }
        break;
    case PreloadAttachResult::None:
        break;
    }

    return true;
}

ResourceManager::Callbacks App::BuildResourceManagerCallbacks()
{
    // clang-format off
    return {
        .invalidate = [this]() {
            Invalidate();
        },
        .set_timer = [this](UINT_PTR id, UINT ms) {
            SetTimer(hwnd_, id, ms, nullptr);
        },
        .kill_timer = [this](UINT_PTR id) {
            KillTimer(hwnd_, id);
        },
        .get_content_width = [this]() -> float {
            return renderer_.GetTheme().ContentWidth(GetMarkdownPaneWidth());
        },
        .get_viewport_height = [this]() -> float {
            return GetPaneLayout().md_rect.height;
        },
        .get_indent_width = [this]() -> float {
            return renderer_.GetTheme().indent_width;
        },
        .recompute_layout = [this]() {
            layout_service_->RecomputeAfterDiagram(state_.document.doc, state_.document.layout_cache, renderer_.GetTheme());
        },
        .recompute_layout_anchored = [this]() {
            EnsureScrollTarget();
            layout_service_->RecomputeAfterDiagram(state_.document.doc, state_.document.layout_cache, renderer_.GetTheme());
            const auto layout = GetPaneLayout();
            EmitEffect(effect::SyncMaxScroll{ layout.md_rect.height });
            Invalidate();
        },
    };
    // clang-format on
}

SearchBarController::Callbacks App::BuildSearchBarCallbacks()
{
    // clang-format off
    return {
        .invalidate = [this]() {
            Invalidate();
        },
        .invalidate_search_bar = [this]() {
            const auto& layout = GetPaneLayout();
            const auto& r = layout.md_rect;
            const PaneRect search_area{ r.x, r.y + r.height - SEARCH_BAR_HEIGHT, r.width, SEARCH_BAR_HEIGHT };
            InvalidatePane(search_area);
        },
        .set_timer = [this](UINT_PTR id, UINT ms) {
            SetTimer(hwnd_, id, ms, nullptr);
        },
        .kill_timer = [this](UINT_PTR id) {
            KillTimer(hwnd_, id);
        },
        .focus_select_all = [this]() {
            EmitEffect(effect::PostWindowMessage{ app_msg::SEARCH_FOCUS, app_param::SEARCH_FOCUS_SELECT_ALL, 0 });
        },
        .focus_set_caret = [this](int pos) {
            EmitEffect(effect::PostWindowMessage{ app_msg::SEARCH_FOCUS, app_param::SEARCH_FOCUS_SET_CARET, static_cast<LPARAM>(pos) });
        },
        .focus_set_selection = [this](int anchor, int caret) {
            EmitEffect(effect::PostWindowMessage{ app_msg::SEARCH_FOCUS, app_param::SEARCH_FOCUS_SET_SELECTION, app_param::MakeSearchSelectionLParam(anchor, caret) });
        },
        .unfocus = [this]() {
            EmitEffect(effect::PostWindowMessage{ app_msg::SEARCH_UNFOCUS, 0, 0 });
        },
        .get_md_pane_height = [this]() -> float {
            return GetPaneLayout().md_rect.height;
        },
        .on_scroll_changed = [this](float md_pane_height) {
            EmitEffect(effect::SyncMaxScroll{ md_pane_height });
            InvalidateHitPositions();
            resource_manager_.ScheduleBitmapManage();
        },
        .on_wrap_around = [] {
            MessageBeep(MB_OK);
        },
    };
    // clang-format on
}
