#include "app.h"
#include "app_constants.h"
#include "ascii_util.h"
#include "clipboard_util.h"
#include "document_utils.h"
#include "file_dialog_service.h"
#include "i18n.h"
#include "layout_computer.h"
#include "pane_layout.h"
#include "profiler.h"
#include "reducer.h"
#include "resource.h"
#include "search_bar_controller.h"
#include "tooltip.h"
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

// ズームの対象。ペインの指定が無ければ本文 (スプリッタの上も本文)。
ZoomLevel& ZoomOf(AppState& state, std::optional<PaneTarget> pane) noexcept
{
    return pane ? state.view.panes.SidePaneZoom(*pane) : state.view.viewport.Zoom();
}

} // namespace

bool App::Init(HWND hwnd)
{
    hwnd_ = hwnd;

    // 書式とブラシを 1 回で作るため、保存済みテーマ/ズームを Renderer 初期化前に読む。
    theme_service_.LoadDarkMode();
    state_.view.viewport.Zoom() = theme_service_.LoadZoomLevel();
    if (!renderer_.Init(hwnd_, theme_service_.CreateTheme(state_.view.viewport.Zoom()))) {
        return false;
    }

    // Mermaid 共有 scheduler_ と詰まり合わないよう独立して立ち上げる。
    layout_scheduler_.Init(LayoutWorkerCount(std::thread::hardware_concurrency()));
    renderer_.SetLayoutScheduler(&layout_scheduler_);

    // PixelToDip 用に DPI スケールをキャッシュ（OnDpiChanged でも更新する）。
    state_.window.cached_dpi_scale = DpiScaleFrom(static_cast<float>(GetDpiForWindow(hwnd_)));

    InitSharedScheduler();

    file_cache_.SetCacheDir(config_.GetConfigPath(L"MermaidCache"));
    file_cache_.Init(MermaidRenderer::EstimateWorkerDpr(), scheduler_);

    clipboard_manager_.Init(hwnd_, &mermaid_renderer_, renderer_.GetWICFactory(), [this](std::wstring_view m) { ShowToast(m); });

    resource_manager_.Init(
        ResourceManagerDeps{
            .doc = &state_.document.doc,
            .cache = &state_.document.layout_cache,
            .viewport = &state_.view.viewport,
            .image_loader = &image_loader_,
            .mermaid = &mermaid_renderer_,
            .mermaid_file_cache = &file_cache_,
        },
        host_);

    mermaid_renderer_.Init(hwnd_, renderer_.GetWICFactory(), file_cache_, scheduler_, app_msg::MERMAID_DISK_LOADED,
                           config_.GetConfigPath(L"WebView2Data"), [this]() {
        // 結果を待っていた図を要求し直して反映する。
        resource_manager_.RequestMermaidRenders();
    });

    if (!image_loader_.Init(renderer_.GetWICFactory())) {
        MENDO_TRACE("ImageLoader::Init failed (WIC factory unavailable). Image rendering disabled.");
    }
    image_loader_.InitAsync(hwnd_, app_msg::IMAGE_LOADED, scheduler_);

    // D2D デバイスロストでレンダーターゲットが再作成されたら、各ローダーへ伝搬する。
    renderer_.SetDeviceLostCallback([this](ID2D1RenderTarget* new_rt) {
        mermaid_renderer_.SetRenderTarget(new_rt);
        image_loader_.SetRenderTarget(new_rt);
        // DiagramEntry::bitmap が新 RT で描画拒否されるのを防ぐ。ApplyCachedImages は
        // bitmap 残存ノードをスキップするため、LoadImages の前に破棄する必要がある。
        state_.document.layout_cache.InvalidateAllDiagramBitmaps();
        resource_manager_.LoadImages();
    });

    RestoreTheme();

    cursors_.Load();

    state_.window.titlebar.UpdateLayout(renderer_.GetSize().width);

    RestorePaneState();

    // フォルダの列挙はネットワークドライブなどで数百 ms かかるので、描画を待たせずに別スレッドで行う。
    // 共有 scheduler の画像デコード待ちの後ろに並ばないよう専用スレッドにする。
    state_.file_explorer.SetAsyncLister([hwnd = hwnd_, slot = file_listing_](std::pmr::wstring dir, uint32_t request) {
        std::thread([hwnd, slot, dir = std::move(dir), request] {
            auto entries = FileExplorer::List(dir);
            {
                // 先に始めた遅い列挙が、まだ受け取っていない新しい結果を上書きしないようにする。
                const std::lock_guard lock(slot->mutex);
                if (!slot->result || slot->result->first < request) {
                    slot->result.emplace(request, std::move(entries));
                }
            }
            ::PostMessageW(hwnd, app_msg::FILE_LISTED, 0, 0);
        }).detach();
    });
    // 起動時に表示するなら、D3D デバイスの作成と並行して列挙を始めておく。
    if (state_.view.panes.IsSidePaneVisible(PaneTarget::File)) {
        state_.file_explorer.GetEntries();
    }

    tooltip_.Init(hwnd_);
    tooltip_.ApplyDarkMode(theme_service_.IsDarkMode());

    state_.search.search_bar_ctrl.Init(state_.search.search_state, state_.view.viewport, state_.document.layout_cache, host_);
    return true;
}

void App::InitSharedScheduler()
{
    scheduler_.Init(SharedWorkerCount(std::thread::hardware_concurrency()));
}

bool App::CreateRenderDevice()
{
    renderer_.PrepareChrome(BuildTitleBarRenderState(), BuildSidePaneState(GetPaneLayout()));
    if (!renderer_.CreateDevice()) {
        return false;
    }
    // 作成前に始めたデコードは WIC ビットマップなので、取り消さずに新しい RT で使う。
    auto* const rt = renderer_.GetRenderTarget();
    mermaid_renderer_.SetRenderTarget(rt);
    image_loader_.SetRenderTarget(rt);
    // デバイスを待つ間に終わった読み込みを、完了通知を待たずに最初のフレームへ入れる。
    // 届いた後で入ると高さが変わって表示が跳ねるので、時間予算で切らずに全部入れる。
    constexpr auto kNoDeadline = std::chrono::steady_clock::time_point::max();
    image_loader_.ProcessCompletedDecodes(kNoDeadline);
    mermaid_renderer_.ProcessDiskLoads(kNoDeadline);
    return true;
}

void App::RestoreTheme()
{
    state_.theme = &renderer_.GetTheme();
    if (theme_service_.IsDarkMode()) {
        ApplyDarkModeToWindow(hwnd_, true);
    }
}

void App::RestorePaneState()
{
    const auto s = session_.LoadPaneState();
    auto& panes = state_.view.panes;
    panes.SetSidePaneVisible(PaneTarget::File, s.show_file);
    panes.SetSidePaneVisible(PaneTarget::Toc, s.show_toc);
    panes.SetSidePaneWidth(PaneTarget::File, s.file_width);
    panes.SetSidePaneWidth(PaneTarget::Toc, s.toc_width);
    panes.SidePaneZoom(PaneTarget::File) = s.file_zoom;
    panes.SidePaneZoom(PaneTarget::Toc) = s.toc_zoom;
}

void App::LoadInitialDocument()
{
    // small file は preload がここまでに完了している場合が多い。直後の UpdateWindow が
    // 同期 WM_PAINT を発行するため、ここで取り込んでおかないと初回フレームが空になる。
    using PreloadAttachResult = FileLoadService::PreloadAttachResult;
    switch (file_load_service_.AttachPreload(hwnd_, app_msg::PARSE_COMPLETE)) {
    case PreloadAttachResult::AppliedSync:
        OnParseComplete();
        return;
    case PreloadAttachResult::AttachedAsync:
        ScheduleLoadingAnimation();
        return;
    case PreloadAttachResult::None:
        LoadHelpDocument();
        return;
    }
    std::unreachable();
}

DipPoint App::PixelToDip(int px, int py) const noexcept
{
    return ::PixelToDip(px, py, state_.window.cached_dpi_scale);
}

const PaneLayout& App::GetPaneLayout()
{
    return EnsurePaneLayout(state_);
}

void App::UpdateTitleBar()
{
    auto title = BuildTitleString(state_.document.doc.GetFilePath(), state_.view.viewport.Zoom().Percent());
    if (title == state_.cached_title_text) {
        return;
    }
    SetWindowTextW(hwnd_, title.c_str());
    state_.cached_title_text = std::move(title);
    Invalidate();
}

PaneZone App::PaneAtPoint(float dip_x)
{
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
            .hovered_index = state_.view.hover.side_index[std::to_underlying(t)],
            .show = panes.IsSidePaneVisible(t),
            .hovered_button = state_.view.hover.side_button[std::to_underlying(t)],
            .reveal_enabled = SidePaneHeaderButtonsFor(state_, t).reveal_enabled,
            .zoom = panes.SidePaneZoom(t).Value(),
            .item_height = SidePaneItemHeight(state_, t),
        };
    };
    return SidePaneState{
        .panes = {
                  make_side_pane(PaneTarget::File, layout.file_rect),
                  make_side_pane(PaneTarget::Toc, layout.toc_rect),
                  },
        .file_entries = panes.IsSidePaneVisible(PaneTarget::File) ? state_.file_explorer.GetEntries() : kNoEntries,
        .toc_entries = state_.document.doc.GetToc(),
        .nodes = state_.document.doc.GetNodes(),
        .active_toc_index = state_.view.active_toc_index,
    };
}

TitleBarRenderState App::BuildTitleBarRenderState() const
{
    const auto& panes = state_.view.panes;
    return TitleBarRenderState{
        .bar = state_.window.titlebar,
        .title_text = state_.cached_title_text,
        .window_width = state_.pane_layout_cache.WindowWidth(),
        .hovered_zone = state_.view.hover.titlebar,
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
    // 最小化中は見えないので描かない。描くと TrimForMinimize で手放したキャッシュを作り直してしまう。
    // 復元時は Win32Window::OnSize が描き直させる。
    if (IsIconic(hwnd_)) {
        EndPaint(hwnd_, &ps);
        return;
    }

    const auto& layout = GetPaneLayout();
    const bool show_loading = file_load_service_.IsLoading();
    if (!show_loading) {
        EnsureFrameLayout(layout);
        // 計測で見出しの Y が決まった後、目次ペインを組む前に決め直す。
        SyncActiveTocIndex(state_);
    }

    const auto gs = ResolveGestureOverlay(state_.interaction.gesture, state_.interaction.swipe_detector);
    const SidePaneState sp = BuildSidePaneState(layout);
    const TitleBarRenderState tb = BuildTitleBarRenderState();
    const ToastRenderState ts{ .message = state_.interaction.toast_message };

    using FrameResult = Renderer::FrameResult;
    FrameResult result = FrameResult::Done;
    if (show_loading) {
        result = renderer_.DrawLoading(file_load_service_.GetLoadingAngle(), layout.md_rect, sp, tb, gs, ts);
    }
    else {
        SyncRendererSearchMatches();

        auto sb = state_.search.search_bar_ctrl.BuildRenderState();
        sb.hovered = state_.view.hover.search_bar;
        sb.caret_visible = state_.window.window_active;
        const BlockHScrollContext h_scroll{
            .scroll_x = &state_.view.block_scroll_x,
            .hovered_block = state_.view.hover.h_block,
            .drag_block = state_.view.HDragNode(),
        };
        result = renderer_.Render({
            .nodes = state_.document.doc.GetNodes(),
            .cache = state_.document.layout_cache,
            .selection = state_.view.viewport.GetSelection(),
            .md_pane_rect = layout.md_rect,
            .side_panes = sp,
            .titlebar = tb,
            .gesture = gs,
            .toast = ts,
            .search_bar = sb,
            .scroll_y = state_.view.viewport.GetScrollY(),
            .nav_hovered = state_.view.hover.nav,
            .hovered = state_.view.hover.overlay_button,
            .hovered_diagram = state_.view.hover.diagram,
            .can_go_back = state_.view.nav_history.CanGoBack(),
            .can_go_forward = state_.view.nav_history.CanGoForward(),
            .block_h_scroll = h_scroll,
        });
    }

    EndPaint(hwnd_, &ps);
    // スクロール・リフロー・ズーム・ペイン開閉などで、止まったカーソルの下の内容が変わる。
    // 経路ごとに引き直しを要求させず、描いた後に毎回引き直す。変化が無ければ reducer が何もしない。
    // ロード中は旧文書に対して引くことになるので、終わった後の描画に任せる。
    if (!show_loading) {
        RefreshHoverAtCursor();
    }
    switch (result) {
    case FrameResult::Done:
        break;
    case FrameResult::RedrawNow:
        Invalidate();
        break;
    case FrameResult::RetryLater:
        SetTimer(app_timer::Id::DEVICE_RECREATE_RETRY, app_timer::DEVICE_RECREATE_RETRY_MS);
        break;
    }
    MENDO_FRAME_MARK();
}

void App::EnsureFrameLayout(const PaneLayout& layout)
{
    const bool updated = layout_service_.EnsureVisibleLayout(state_.document.doc, state_.document.layout_cache, layout.md_rect.width, layout.md_rect.height);
    if (updated) {
        ClampScrollToContent();
        // 未計測領域に入った。スクロール先を先読み計測させ、以降のフレームでの同期計測を減らす。
        RequestDeferredLayout();
    }
}

void App::PrepareFrame()
{
    if (!IsRenderReady() || IsIconic(hwnd_) || file_load_service_.IsLoading()) {
        return;
    }
    EnsureFrameLayout(GetPaneLayout());
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
    mermaid_renderer_.ProcessDiskLoads();
}

void App::OnFileListed()
{
    std::optional<std::pair<uint32_t, std::pmr::vector<FileEntry>>> listed;
    {
        const std::lock_guard lock(file_listing_->mutex);
        listed.swap(file_listing_->result);
    }
    if (!listed || !state_.file_explorer.ApplyListing(listed->first, std::move(listed->second))) {
        return;
    }
    if (state_.file_explorer.TakeDeferredReveal()) {
        Dispatch(FilePaneRevealCurrentFileAction{});
    }
    Invalidate();
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
    auto effects = Reduce(state_, action);
    Execute(effects);
}

void App::OnDropFiles(HDROP hDrop)
{
    const UINT required = DragQueryFileW(hDrop, 0, nullptr, 0);
    if (required > 0) {
        std::pmr::wstring path(required, L'\0');
        if (DragQueryFileW(hDrop, 0, path.data(), required + 1)) {
            Dispatch(OpenPathAction{ std::move(path) });
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
        Dispatch(SwipeOverlayTimeoutAction{});
        return;
    case TOAST:
        Dispatch(ToastExpiredAction{});
        return;
    case TOOLTIP:
        KillTimer(id);
        tooltip_.Show();
        return;
    case LOADING_ANIM:
        // 最初の 1 回は ScheduleLoadingAnimation の遅延の後に届く。
        if (!file_load_service_.IsLoading()) {
            if (file_load_service_.IsAsyncLoading()) {
                StartLoadingAnimation();
            }
            else {
                KillTimer(id);
            }
            return;
        }
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
    case DEVICE_RECREATE_RETRY:
        KillTimer(id);
        Invalidate();
        return;
    case FILE_RELOAD_DEBOUNCE:
        KillTimer(id);
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
    state_.interaction.toast_message = message;
    SetTimer(app_timer::Id::TOAST, app_timer::TOAST_DURATION_MS);
    Invalidate();
}

void App::ShellOpenAsync(std::pmr::wstring target)
{
    // シェル拡張が COM で動くので STA に入る。共有 scheduler の worker は MTA なので専用スレッドで開く。
    std::thread([target = std::move(target)] {
        const HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
        // 戻ったらすぐスレッドを畳むので、起動の受け渡しを非同期に残させない。
        SHELLEXECUTEINFOW info{
            .cbSize = sizeof(info),
            .fMask = SEE_MASK_NOASYNC,
            .lpVerb = L"open",
            .lpFile = target.c_str(),
            .nShow = SW_SHOWNORMAL,
        };
        ShellExecuteExW(&info);
        if (SUCCEEDED(hr)) {
            CoUninitialize();
        }
    }).detach();
}

void App::StartFileWatch(const std::pmr::wstring& path)
{
    file_watcher_.StartWatching(path, [this] {
        KillTimer(app_timer::Id::FILE_RELOAD_DEBOUNCE);
        SetTimer(app_timer::Id::FILE_RELOAD_DEBOUNCE, app_timer::FILE_RELOAD_DEBOUNCE_MS);
    });
}

void App::OpenFileDialog()
{
    if (auto path = file_dialog_service::OpenMarkdownFileDialog(hwnd_); !path.empty()) {
        Dispatch(OpenPathAction{ std::move(path) });
    }
}

void App::Execute(const SideEffect& effect)
{
    // clang-format off
    std::visit(overloaded{
        // ---- Ui ----
        [this](const effect::InvalidateWindow&) {
            Invalidate();
        },
        [this](const effect::SetCapture&) {
            ::SetCapture(hwnd_);
        },
        [this](const effect::ReleaseCapture&) {
            // ポップアップ (設定ダイアログなど) が持っているキャプチャまで外さない。
            if (::GetCapture() == hwnd_) {
                ::ReleaseCapture();
            }
        },
        [this](const effect::ClipboardWrite& ev) {
            WriteClipboardText(hwnd_, ev.text);
        },
        [this](const effect::ClipboardWriteHtml& ev) {
            WriteClipboardHtml(hwnd_, ev.html, ev.plain);
        },
        [this](const effect::ShowTooltip& ev) {
            if (tooltip_.Update(ev.target)) {
                SetTimer(app_timer::Id::TOOLTIP, TOOLTIP_DELAY_MS);
            }
            else if (ev.target.IsEmpty()) {
                KillTimer(app_timer::Id::TOOLTIP);
            }
        },
        [this](const effect::ClearTooltip&) {
            tooltip_.Clear();
            KillTimer(app_timer::Id::TOOLTIP);
        },
        [this](const effect::ShowContextMenu& ev) {
            OnContextMenu(ev.screen_x, ev.screen_y);
        },
        [](const effect::Beep&) static {
            MessageBeep(MB_OK);
        },
        // ---- Window ----
        [this](const effect::SearchFocus& ev) {
            PostMessageW(hwnd_, app_msg::SEARCH_FOCUS, 0, app_param::MakeSearchSelectionLParam(ev.anchor, ev.caret));
        },
        [this](const effect::SearchUnfocus& ev) {
            const WPARAM wp = ev.clear_text ? app_param::SEARCH_UNFOCUS_FILE_SWITCH : app_param::SEARCH_UNFOCUS_CLOSE;
            PostMessageW(hwnd_, app_msg::SEARCH_UNFOCUS, wp, 0);
        },
        [this](const effect::SetWindowPosition& ev) {
            SetWindowPos(hwnd_, nullptr, ev.x, ev.y, ev.cx, ev.cy, SWP_NOZORDER | SWP_NOACTIVATE);
        },
        [this](const effect::ApplyZoom&) {
            ApplyZoom();
        },
        [this](const effect::ToggleDarkMode&) {
            ToggleDarkMode();
        },
        [this](const effect::PerformResizeEnd&) {
            OnResizeEnd();
        },
        [this](const effect::PerformSizingUpdate&) {
            ClampScrollToContent();
            Invalidate();
        },
        [this](const effect::RendererResize& ev) {
            renderer_.Resize(ev.width, ev.height, ev.live);
        },
        [this](const effect::RendererSetDpi& ev) {
            renderer_.SetDpi(ev.dpi);
        },
        // ---- Navigation ----
        [this](const effect::LoadFile& ev) {
            LoadMarkdownFile(ev.path);
        },
        [this](const effect::ReloadFile&) {
            ReloadCurrentFile();
        },
        [this](const effect::OpenFileDialog&) {
            OpenFileDialog();
        },
        [this](const effect::CancelOtherFileLoad&) {
            CancelOtherFileLoad();
        },
        // ---- Layout ----
        [this](const effect::BitmapManage&) {
            resource_manager_.ScheduleBitmapManage();
        },
        // ---- Timer ----
        [this](const effect::SetTimer& ev) {
            SetTimer(ev.id, ev.ms);
        },
        [this](const effect::KillTimer& ev) {
            KillTimer(ev.id);
        },
    }, effect);
    // clang-format on
}

void App::SaveSession()
{
    // SaveScrollPosition だけガード外に置くと、Help 表示中に終了したとき
    // 前回 LastFilePath に Help の node index が紐付き、次回起動時に誤位置へジャンプする。
    const auto& doc = state_.document.doc;
    if (doc.HasBackingFile()) {
        session_.SaveLastFilePath(doc.GetFilePath());
        if (const auto t = SnapshotVisibleTarget(state_); t.IsValid()) {
            session_.SaveScrollPosition(t.node, t.offset);
        }
    }

    const auto& panes = state_.view.panes;
    session_.SavePaneState({
        .show_file = panes.IsSidePaneVisible(PaneTarget::File),
        .show_toc = panes.IsSidePaneVisible(PaneTarget::Toc),
        .file_width = panes.GetSidePaneWidth(PaneTarget::File),
        .toc_width = panes.GetSidePaneWidth(PaneTarget::Toc),
        .file_zoom = panes.SidePaneZoom(PaneTarget::File),
        .toc_zoom = panes.SidePaneZoom(PaneTarget::Toc),
    });

    session_.SaveLanguage(i18n::ConfigLangKey());
    // 個別 Save 呼び出しでは write が遅延されるため、終了前に明示 flush で
    // すべての設定値を 1 度のディスク書き込みにまとめる。
    config_.Flush();
}

void App::OnDestroy()
{
    // ログオフ時は OS に打ち切られる前に済ませたいので、スレッドの join より先に保存する。
    SaveSession();

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

    for (UINT_PTR id = std::to_underlying(app_timer::kFirstTimer); id <= std::to_underlying(app_timer::kLastTimer); ++id) {
        ::KillTimer(hwnd_, id);
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

int App::HitTestSearchInputPos(const SearchBarLayout& sbl, float dip_x) const
{
    return renderer_.HitTestSearchInput(state_.search.search_bar_ctrl.GetQueryWide(), dip_x - sbl.text_left(), sbl.text_width());
}

RECT App::GetSearchEditRect()
{
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

void App::RestoreScrollPosition()
{
    const auto pos = session_.LoadScrollPosition();
    if (pos.node >= 0) {
        state_.view.pending_restore = { pos.node, static_cast<float>(pos.offset) };
    }
}

void App::ApplyZoom()
{
    renderer_.ApplyZoom(theme_service_.CreateTheme(state_.view.viewport.Zoom()));
    RelayoutViewport(RelayoutResources::Diagrams);
    UpdateTitleBar();
    theme_service_.SaveZoomLevel(state_.view.viewport.Zoom());
}

void App::ToggleDarkMode()
{
    const bool dark = theme_service_.ToggleDarkMode();
    renderer_.SetTheme(theme_service_.CreateTheme(state_.view.viewport.Zoom()));
    ApplyDarkModeToWindow(hwnd_, dark);
    tooltip_.ApplyDarkMode(dark);
    // メモリキャッシュはキーにダークモードを含むので消さず、往復時に再利用する。描画待ちは旧テーマの分なので捨てる。
    mermaid_renderer_.DropQueued();
    RelayoutViewport(RelayoutResources::Diagrams);
    theme_service_.SaveDarkMode();
}

void App::OnContextMenu(int screen_x, int screen_y)
{
    POINT client_pt{ screen_x, screen_y };
    ScreenToClient(hwnd_, &client_pt);
    const auto dip = PixelToDip(client_pt.x, client_pt.y);

    const auto& selection = state_.view.viewport.GetSelection();
    const auto& panes = state_.view.panes;
    const PaneZone zone = PaneAtPoint(dip.x);
    const std::optional<PaneTarget> zoom_pane = ToPaneTarget(zone);
    const ContextMenuParams params{
        .env = MakePopupEnv(),
        .screen_x = screen_x,
        .screen_y = screen_y,
        .can_go_back = state_.view.nav_history.CanGoBack(),
        .can_go_forward = state_.view.nav_history.CanGoForward(),
        // ヘルプは埋め込みリソースで、エディタで開くファイルが無い。
        .has_file = state_.document.doc.HasBackingFile(),
        .has_selection = selection.active && selection.start_node >= 0,
        .dark_mode_checked = theme_service_.IsDarkMode(),
        .file_pane_checked = panes.IsSidePaneVisible(PaneTarget::File),
        .toc_pane_checked = panes.IsSidePaneVisible(PaneTarget::Toc),
        .show_file_items = zone == PaneZone::MdPane,
        .zoom = &ZoomOf(state_, zoom_pane),
        .on_zoom = [this, zoom_pane](ZoomDirection direction) { Dispatch(ZoomAction{ direction, 1, zoom_pane }); },
    };

    switch (ContextMenu::Show(hwnd_, params)) {
    case IDM_NAV_BACK:
        Dispatch(NavigateBackAction{});
        break;
    case IDM_NAV_FORWARD:
        Dispatch(NavigateForwardAction{});
        break;
    case IDM_EDIT_FILE: {
        const auto& file_path = state_.document.doc.GetFilePath();
        if (IsEditableTextFile(file_path)) {
            ShellOpenAsync(file_path);
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

PopupEnv App::MakePopupEnv() const noexcept
{
    return {
        .d2d_factory = renderer_.GetD2DFactory(),
        .dwrite_factory = renderer_.GetDWriteFactory(),
        .theme = &renderer_.GetTheme(),
        .dpi_scale = state_.window.cached_dpi_scale,
        .modal_loop = &modal_loop_,
    };
}

void App::ShowSettingsDialog()
{
    const SettingsDialogParams params{
        .env = MakePopupEnv(),
        .diagram_cache_bytes = file_cache_.TotalSize(),
        .on_language = [this](const i18n::Strings* chosen) { ApplyLanguage(chosen); },
        .on_clear_cache = [this] { file_cache_.Clear(); },
    };
    SettingsDialog::Show(hwnd_, params);
}

void App::ApplyLanguage(const i18n::Strings* chosen)
{
    // 表示する文言が変わらなければ、選んだ値を保存するだけ。
    if (!i18n::SetLanguage(chosen)) {
        return;
    }
    if (IsHelpPath(state_.document.doc.GetFilePath())) {
        ShowHelpResource();
    }
    Invalidate();
}

float App::MdMaxScroll()
{
    EnsurePaneLayout(state_);
    return ::MdMaxScroll(state_);
}

void App::ClampScrollToContent()
{
    state_.view.viewport.ClampScroll(MdMaxScroll());
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

bool App::RequestDeferredLayout()
{
    if (!state_.document.layout_cache.HasDirtyNodes()) {
        return false;
    }
    deferred_layout_pending_ = true;
    return true;
}

bool App::HasIdleWork()
{
    return deferred_layout_pending_ || state_.search.search_state.IsSearching() || state_.document.layout_cache.HasReleasedLayouts() ||
           image_loader_.HasLeftoverUploads() || mermaid_renderer_.HasLeftoverDiskLoads();
}

// 1 回に 1 種類だけ進めてメッセージループへ戻り、入力と描画を先に配らせる。
// 画面に出る転送、待たれている検索、先読みの計測の順。捨てたレイアウトの解放は渡すだけなので先に済ませる。
void App::RunIdleWork()
{
    DisposeReleasedLayouts();
    if (image_loader_.HasLeftoverUploads() || mermaid_renderer_.HasLeftoverDiskLoads()) {
        // 両方で 1 回分の持ち時間を分け合う。
        const auto deadline = std::chrono::steady_clock::now() + kUploadSliceBudget;
        image_loader_.ProcessCompletedDecodes(deadline);
        mermaid_renderer_.ProcessDiskLoads(deadline);
        return;
    }
    if (state_.search.search_state.IsSearching()) {
        Dispatch(SearchContinueAction{});
        return;
    }
    if (deferred_layout_pending_) {
        OnDeferredLayout();
    }
}

// ロード・ズーム・リサイズでは画面外にもレイアウトが作られ (初回の MeasureTable は全セル)、
// 本文幅が広がると画像の解像度が足りなくなる。スクロールを待たずに BitmapManage で整理させる。
// 遅延レイアウトが残っていれば、その完了時 (OnDeferredLayout) に出す。
void App::SettleLayout()
{
    if (!RequestDeferredLayout()) {
        resource_manager_.ScheduleBitmapManage();
    }
}

void App::RelayoutViewport(RelayoutResources resources)
{
    MENDO_PROFILE("RelayoutViewport");
    const PaneRect md = GetPaneLayout().md_rect;
    // 可視範囲を即時計測し、scroll_target があれば scroll_y を再評価する。
    layout_service_.ViewportLayout(state_.document.doc, state_.document.layout_cache, md.width, md.height);
    ClampScrollToContent();
    switch (resources) {
    case RelayoutResources::Diagrams:
        resource_manager_.RequestMermaidRenders();
        break;
    case RelayoutResources::DiagramsBatched:
        resource_manager_.ScheduleMermaidBatch();
        break;
    case RelayoutResources::ImagesAndDiagrams:
        resource_manager_.LoadImages();
        resource_manager_.RequestMermaidRenders();
        break;
    }
    Invalidate();
    SettleLayout();
}

void App::OnResizeEnd()
{
    MENDO_PROFILE("OnResizeEnd");

    deferred_layout_pending_ = false;
    state_.view.viewport.EnsureScrollTarget(state_.document.layout_cache);
    RelayoutViewport(RelayoutResources::DiagramsBatched);
}

void App::OnDeferredLayout()
{
    MENDO_PROFILE("OnDeferredLayout");

    // 表やコードブロックが多いと同じノード数でも時間が大きく違うので、件数ではなく時間で区切る。
    // 件数で細かく区切ると、区切るたびに走査・Y の組み直し・ワーカーの待ち合わせが挟まる
    // (test.md x4 の全計測で、32 件ずつだと一度に測る 165ms が 290ms になった)。
    constexpr auto kDeferredLayoutBudget = std::chrono::milliseconds(4);

    const PaneRect md = GetPaneLayout().md_rect;
    const bool more = layout_service_.ProcessDirtyBatch(
        state_.document.doc, state_.document.layout_cache, md.width, std::chrono::steady_clock::now() + kDeferredLayoutBudget,
        LayoutService::ViewportLimit{ md.height, ResourceManager::EVICT_BUFFER_SCREENS });

    // 中間バッチではクランプしない (ClampScrollToContent の注記)。スクロールバーのドラッグは
    // 移動のたびにその時点の上限で位置を決めるので、ここで合わせる必要はない。
    if (more) {
        return;
    }
    deferred_layout_pending_ = false;

    // 同期ディスク I/O + PNG デコードで UI スレッドを長時間ブロックしないよう、
    // Mermaid ファイルキャッシュの読み込みは時間予算付きバッチに回す。
    resource_manager_.ScheduleMermaidBatch();
    resource_manager_.ScheduleBitmapManage();

    ClampScrollToContent();
    Invalidate();
}

void AppHost::SetTimer(app_timer::Id id, UINT ms)
{
    app_.SetTimer(id, ms);
}

void AppHost::KillTimer(app_timer::Id id)
{
    app_.KillTimer(id);
}

float AppHost::MdContentWidth()
{
    return app_.MdContentWidth();
}

float AppHost::MdPaneHeight()
{
    return app_.GetPaneLayout().md_rect.height;
}

float AppHost::MdMaxScroll()
{
    return app_.MdMaxScroll();
}

const Theme& AppHost::GetTheme()
{
    return app_.renderer_.GetTheme();
}

void AppHost::RecomputeLayout(layout::HeightChangeRange changed)
{
    app_.layout_service_.RecomputeAfterDiagram(
        app_.state_.document.doc,
        app_.state_.document.layout_cache,
        app_.renderer_.GetTheme(),
        changed);
    app_.ClampScrollToContent();
    app_.Invalidate();
}

void AppHost::EnsureNodeLayout(int node_index)
{
    app_.layout_service_.EnsureNodeLayout(
        app_.state_.document.doc, app_.state_.document.layout_cache,
        app_.GetPaneLayout().md_rect.width, node_index);
}
