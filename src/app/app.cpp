#include "app.h"
#include "app_constants.h"
#include "ascii_util.h"
#include "clipboard_util.h"
#include "document_utils.h"
#include "file_io.h"
#include "i18n.h"
#include "layout_computer.h"
#include "layout.h"
#include "pane_layout.h"
#include "profiler.h"
#include "reload_flow.h"
#include "resource.h"
#include "string_convert.h"
#include "titlebar.h"
#include "tooltip.h"
#include "ui_constants.h"
#include "win32_util.h"
#include "window.h"
#include <array>
#include <cstddef>
#include <memory_resource>
#include <thread>
#include <utility>

namespace {

bool IsEditableTextFile(std::wstring_view path)
{
    return path_util::IsMarkdownFile(path) || ascii_util::iequal(path_util::ExtensionView(path), L".txt");
}

KeyDownEvent CurrentKeyDownEvent(WPARAM key) noexcept
{
    return { static_cast<int>(key), IsKeyDown(VK_CONTROL), IsKeyDown(VK_SHIFT), IsKeyDown(VK_MENU) };
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
    state_.view.viewport.Zoom() = session::LoadZoomLevel(config_);
    theme_service_.Init(session::LoadDarkMode(config_), state_.view.viewport.Zoom());
    const Theme& theme = theme_service_.GetTheme();
    if (!renderer_.Init(hwnd_, theme)) {
        return false;
    }
    measurer_.SetFactory(renderer_.GetDWriteFactory());
    measurer_.SetInlineMath(&inline_math_);
    inline_math_.SetWake([hwnd = hwnd_] { ::PostMessageW(hwnd, app_msg::INLINE_MATH_REQUESTED, 0, 0); });
    if (!layout_engine_.Init(&measurer_, theme)) {
        return false;
    }

    measurer_.SetScheduler(&layout_scheduler_);

    // PixelToDip 用に DPI スケールをキャッシュ（DpiChangedAction でも更新する）。
    state_.window.cached_dpi_scale = DpiScaleFrom(static_cast<float>(GetDpiForWindow(hwnd_)));

    file_cache_.SetCacheDir(config_.GetConfigPath(L"MermaidCache"));
    file_cache_.Init(MermaidRenderer::EstimateWorkerDpr(), scheduler_);

    clipboard_manager_.Init(hwnd_, &mermaid_renderer_, renderer_.GetWICFactory(), [this](std::wstring_view m) { ShowToast(m); });

    resource_manager_.Init(state_, image_loader_, mermaid_renderer_, file_cache_, *this);

    mermaid_renderer_.Init(hwnd_, renderer_.GetWICFactory(), file_cache_, scheduler_,
                           config_.GetConfigPath(L"WebView2Data"), [this]() {
        // 結果を待っていた図を要求し直して反映する。
        resource_manager_.RequestMermaidRenders();
    });

    if (!image_loader_.Init(renderer_.GetWICFactory(), hwnd_, app_msg::IMAGE_LOADED, scheduler_)) {
        MENDO_TRACE("ImageLoader::Init failed (WIC factory unavailable). Image rendering disabled.");
    }

    // D2D デバイスロストでレンダーターゲットが再作成されたら、各ローダーへ伝搬する。
    renderer_.SetDeviceLostCallback([this](ID2D1RenderTarget* new_rt) {
        mermaid_renderer_.SetRenderTarget(new_rt);
        image_loader_.SetRenderTarget(new_rt);
        // DiagramEntry::bitmap が新 RT で描画拒否されるのを防ぐ。ApplyCachedImages は
        // bitmap 残存ノードをスキップするため、LoadImages の前に破棄する必要がある。
        state_.document.layout_cache.InvalidateAllDiagramBitmaps();
        resource_manager_.LoadImages();
        // インライン数式の型は旧デバイスのビットマップなので捨てる。寸法は同じなのでレイアウトはそのまま。
        // 見えたときに組を読み直す (ManageInlineMathPages) ので、全部の組は読まない。
        math_masks_ = {};
    });

    if (theme_service_.IsDarkMode()) {
        ApplyDarkModeToWindow(hwnd_, true);
    }

    cursors_.Load();

    state_.window.titlebar.UpdateLayout(renderer_.GetSize().width);

    session::LoadPanes(config_, state_.view.panes);

    // フォルダの列挙はネットワークドライブなどで数百 ms かかるので、描画を待たせずに別スレッドで行う。
    // 共有 scheduler の画像デコード待ちの後ろに並ばないよう専用スレッドにする。
    state_.file_explorer.SetAsyncLister([hwnd = hwnd_, slot = file_listing_](std::wstring dir, uint32_t request) {
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
        state_.file_explorer.EnsureListed();
    }

    tooltip_.Init(hwnd_);
    tooltip_.ApplyDarkMode(theme_service_.IsDarkMode());

    return true;
}

bool App::CreateRenderDevice()
{
    renderer_.PrepareChrome(BuildTitleBarRenderState(), BuildSidePaneState(ComputePaneLayout(state_)));
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
    resource_manager_.OnImagesDecoded(kNoDeadline);
    mermaid_renderer_.ProcessDiskLoads(kNoDeadline);
    return true;
}

void App::LoadInitialDocument()
{
    // 結果は出ていても通知で届く。Win32Window::Create が最初のフレームより前に取り込む。
    if (load_coordinator_.Attach(hwnd_, app_msg::PARSE_COMPLETE)) {
        ScheduleLoadingAnimation();
    }
    else {
        ShowHelpResource();
    }
}

DipPoint App::PixelToDip(int px, int py) const noexcept
{
    return ::PixelToDip(px, py, state_.window.cached_dpi_scale);
}

void App::UpdateTitleBar()
{
    auto title = BuildTitleString(state_.document.doc.GetFilePath(), state_.view.viewport.Zoom().Percent());
    if (title == title_text_) {
        return;
    }
    SetWindowTextW(hwnd_, title.c_str());
    title_text_ = std::move(title);
    Invalidate();
}

PaneZone App::PaneAtPoint(float dip_x)
{
    return DetectPaneZone(dip_x, ComputePaneLayout(state_), theme_service_.GetTheme().splitter_width);
}

SidePaneState App::BuildSidePaneState(const PaneLayout& layout)
{
    const auto& panes = state_.view.panes;
    const bool file_visible = panes.IsSidePaneVisible(PaneTarget::File);
    if (file_visible) {
        state_.file_explorer.EnsureListed();
    }
    static const std::vector<FileEntry> kNoEntries;
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
        .file_entries = file_visible ? state_.file_explorer.GetEntries() : kNoEntries,
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
        .title_text = title_text_,
        .window_width = state_.window.WidthDip(),
        .hovered_zone = state_.view.hover.titlebar,
        .search_active = state_.search.search_state.IsVisible(),
        .file_pane_visible = panes.IsSidePaneVisible(PaneTarget::File),
        .toc_pane_visible = panes.IsSidePaneVisible(PaneTarget::Toc),
        .is_maximized = IsZoomed(hwnd_) != FALSE,
        .window_active = state_.window.window_active,
    };
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

    const auto layout = ComputePaneLayout(state_);
    const bool show_loading = spinner_shown_;
    if (!show_loading) {
        EnsureFrameLayout(layout);
        // 計測で見出しの Y が決まった後、目次ペインを組む前に決め直す。
        SyncActiveTocIndex(state_);
    }

    const auto gs = ResolveGestureOverlay(state_.interaction.gesture, state_.interaction.swipe_detector);
    const SidePaneState sp = BuildSidePaneState(layout);
    const TitleBarRenderState tb = BuildTitleBarRenderState();

    const auto& ss = state_.search.search_state;
    const bool highlight = ss.IsVisible() && ss.IsHighlightEnabled();
    const auto sb = BuildSearchBarRenderState(state_);
    const BlockHScrollContext h_scroll{
        .scroll_x = &state_.view.block_scroll_x,
        .hovered_block = state_.view.hover.h_block,
        .drag_block = state_.view.HDragNode(),
    };
    using FrameResult = Renderer::FrameResult;
    const FrameResult result = renderer_.Render({
        .md = {
            .nodes = state_.document.doc.GetNodes(),
            .cache = state_.document.layout_cache,
            .selection = state_.view.viewport.GetSelection(),
            .md_pane_rect = layout.md_rect,
            .scroll_y = state_.view.viewport.GetScrollY(),
            .hovered = state_.view.hover.overlay_button,
            .hovered_diagram = state_.view.hover.diagram,
            .block_h_scroll = h_scroll,
            .search_matches = highlight ? std::span<const SearchMatch>{ ss.GetMatches() } : std::span<const SearchMatch>{},
            .current_match_index = highlight ? ss.GetCurrentMatchIndex() : -1,
            .search_generation = highlight ? ss.GetGeneration() : 0,
            .inline_math = &math_masks_,
        },
        .side_panes = sp,
        .titlebar = tb,
        .gesture = gs,
        .toast_message = toast_message_,
        .search_bar = sb,
        .nav_hovered = state_.view.hover.nav,
        .can_go_back = state_.view.nav_history.CanGoBack(),
        .can_go_forward = state_.view.nav_history.CanGoForward(),
        .loading_angle = show_loading ? std::optional{ spinner_angle_ } : std::nullopt,
    });

    EndPaint(hwnd_, &ps);
    ManageInlineMathPages();
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
        SettleLayout();
    }
}

void App::PrepareFrame()
{
    if (!IsRenderReady() || IsIconic(hwnd_) || spinner_shown_) {
        return;
    }
    EnsureFrameLayout(ComputePaneLayout(state_));
}

void App::OnMinimize()
{
    renderer_.TrimForMinimize();
}

void App::OnAppImageLoaded()
{
    resource_manager_.OnImagesDecoded();
}

void App::OnMermaidDiskLoaded()
{
    mermaid_renderer_.ProcessDiskLoads();
}

void App::OnInlineMathRequested()
{
    for (const uint64_t key : inline_math_.TakeRequests()) {
        // 計画に無いのは、リロードの前に測って今の文書には無くなった数式だけ。描かずに待ちから外す。
        if (!RequestInlineMathChunk(key, false)) {
            inline_math_.ForgetRequest(key);
        }
    }
}

bool App::RequestInlineMathChunk(uint64_t key, bool wanted)
{
    const auto chunk = inline_math_plan_.ChunkOf(key);
    if (!chunk) {
        return false;
    }
    // 組の残りの数式もまとめて描く (見えていない数式も、次にスクロールしたときには描けている)。
    if (const auto* c = inline_math_plan_.TakeChunk(*chunk, wanted)) {
        inline_math_.MarkRequested(c->items);
        const size_t index = *chunk;
        const uint64_t chunk_key = c->key;
        mermaid_renderer_.RequestMathBatch(
            c->items, chunk_key, [this, index, chunk_key](InlineMathBatchResult result) { OnInlineMathRendered(std::move(result), index, chunk_key); },
            [this, index, chunk_key](size_t bytes) { return ReserveInlineMathPage(index, chunk_key, bytes); });
    }
    return true;
}

bool App::ReserveInlineMathPage(size_t chunk, uint64_t chunk_key, size_t bytes)
{
    // 見えている数式のために頼んだ組でなければ、上限に収まるページだけを受け取る。収まらなければ寸法だけ使い、
    // 見えたときに読み直す。表全体を測ると全部の組が届き、5000 行の表 (数式 10000 個) ではページが約 38MB になる。
    // デコードと GPU への転送の前に決めるので、この表を 2 回目に開くとデコードが 151 枚 → 20 枚になり、全部の寸法が
    // 揃うまでが約 160ms → 約 122ms。組はまとめて頼むので、受け取ると答えてまだ届いていない分も数える。
    if (!inline_math_plan_.IsWanted(chunk, chunk_key) &&
        math_masks_.page_bytes + reserved_math_page_bytes_ + bytes > kInlineMathPageBudget) {
        return false;
    }
    reserved_math_page_bytes_ += bytes;
    return true;
}

void App::ManageInlineMathPages()
{
    // 型の無い (受け取らなかったページ・捨てたページ・デバイスロスト) 数式が見えたら、組をディスクキャッシュから読み直す。
    // 同じ組を頼むのは結果を待つ間 1 回だけなので、同じ数式が何度出てきてもよい。
    for (const uint64_t key : std::exchange(math_masks_.missing, {})) {
        RequestInlineMathChunk(key, true);
    }
    math_masks_.EvictPages(kInlineMathPageBudget);
}

void App::RebuildInlineMathPlan()
{
    // 数式の無い文書でも省かない。test.md x8 (6922 ノード) で 66µs、数式 12000 個で 2.6ms (BM_InlineMathPlanBuild)。
    inline_math_plan_.Build(state_.document.doc.GetNodes(), theme_service_.GetTheme());
}

void App::OnInlineMathRendered(InlineMathBatchResult result, size_t chunk, uint64_t chunk_key)
{
    reserved_math_page_bytes_ -= result.reserved_bytes;
    // 取り消すのは終了するときと ForgetInlineMath のときだけで、どちらも結果を使わない。
    if (result.cancelled) {
        return;
    }
    inline_math_plan_.FinishChunk(chunk, chunk_key, result.failed);
    InlineMathPage* page = nullptr;
    bool lost = false;
    for (const auto& item : result.items) {
        if (!item.metrics.ok()) {
            const auto before = inline_math_.Find(item.key);
            lost |= before && before->ok();
        }
        // ForgetInlineMath の前に頼んだ数式は、もう誰も使わない。
        if (!inline_math_.Resolve(item.key, item.metrics) || !item.metrics.ok() || !result.page) {
            continue;
        }
        if (!page) {
            page = &math_masks_.AddPage(result.page);
        }
        math_masks_.Assign(item.key, InlineMathMask{ page, item.src, item.dest });
    }
    auto& cache = state_.document.layout_cache;
    // 数式の大きさで見ている段落の高さが変わっても、見ている位置を動かさない。
    state_.view.viewport.EnsureScrollTarget(cache);
    // 型を失って読み直した数式が描けなかった (ディスクキャッシュが消えていて、WebView2 でも描けなかった)。
    // 組んだレイアウトはその数式の幅を空けたままなので、原文で組み直す。数式からノードは引けないので全部組み直す。
    if (lost) {
        cache.InvalidateAllLayouts(state_.document.doc.GetNodes());
        RelayoutViewport();
        return;
    }
    // 段落は組の結果が続けて届いても、まとめずに結果ごとに組み直す。数式 2940 個の文書を最小倍率・1600x1400 で開いても、
    // 1 件あたり 1ms 以下、全部で約 4ms (ディスクキャッシュから 12 件が続けて届く場合)。
    // 表は全セルを組み直すので、頼んだ数式が揃ったときか、前に表を組み直してから一定時間たったときだけにする。
    // 5000 行の表 (数式 10000 個) で、結果ごとだと 1 件あたり約 35ms、結果 165 件で計 5.5 秒かかっていた。
    // 間引くと初回 8 回で計 0.31 秒、ディスクキャッシュから読む 2 回目は 2 回で計 0.09 秒。
    // 間引いた表は、次の結果が届かなくても間隔の後で組み直す。
    const auto since = std::chrono::steady_clock::now() - last_math_table_relayout_;
    const bool tables = !inline_math_.HasUnresolved() || since >= kMathTableRelayoutInterval;
    if (!tables) {
        SetTimer(app_timer::Id::INLINE_MATH_TABLES,
                 static_cast<UINT>(std::chrono::ceil<std::chrono::milliseconds>(kMathTableRelayoutInterval - since).count()));
    }
    RelayoutPendingInlineMath(tables);
}

void App::RelayoutPendingInlineMath(bool tables)
{
    if (tables) {
        KillTimer(app_timer::Id::INLINE_MATH_TABLES);
        last_math_table_relayout_ = std::chrono::steady_clock::now();
    }
    if (state_.document.layout_cache.InvalidatePendingMathLayouts(tables)) {
        RelayoutViewport();
    }
    else {
        Invalidate();
    }
}

void App::ForgetInlineMath()
{
    inline_math_.Clear();
    math_masks_ = {};
    mermaid_renderer_.DropQueuedMath();
    // 組の待ちの印も戻す。表を捨てたので、計測が頼み直した数式を組ごとに描かせる。
    RebuildInlineMathPlan();
}

void App::OnFileListed()
{
    std::optional<std::pair<uint32_t, std::vector<FileEntry>>> listed;
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

bool App::OnKeyDown(WPARAM key)
{
    return DispatchKeyAction(app_controller::HandleKeyDown(CurrentKeyDownEvent(key)));
}

bool App::OnSearchEditKeyDown(WPARAM key)
{
    return DispatchKeyAction(app_controller::HandleSearchEditKeyDown(CurrentKeyDownEvent(key)));
}

bool App::DispatchKeyAction(const std::optional<AppAction>& action)
{
    if (!action) {
        return false;
    }
    Dispatch(*action);
    return true;
}

void App::Dispatch(const AppAction& action)
{
    // マウスの移動・ホイール・ドラッグは 1 イベントごとに副作用を積む。一覧の確保と解放は Reduce の 3〜4 割を
    // 占めていた (BM_ReduceEvents) ので、呼び出しごとのスタックの領域から取る。入れ子の Dispatch も自分の領域を持つ。
    // 副作用が持つ文字列はヒープから取る (variant は allocator を受け継がない) ので、領域より長生きしない。
    alignas(std::max_align_t) std::array<std::byte, 1024> buffer;
    std::pmr::monotonic_buffer_resource arena(buffer.data(), buffer.size());
    auto effects = Reduce(state_, action, *this, &arena);
    Execute(effects);
}

void App::OnDropFiles(HDROP hDrop)
{
    const UINT required = DragQueryFileW(hDrop, 0, nullptr, 0);
    if (required > 0) {
        std::wstring path(required, L'\0');
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
        KillTimer(id);
        toast_message_.clear();
        Invalidate();
        return;
    case TOOLTIP:
        KillTimer(id);
        tooltip_.Show();
        return;
    case LOADING_ANIM:
        // 最初の 1 回は ScheduleLoadingAnimation の遅延の後に届く。
        if (!spinner_shown_) {
            if (load_coordinator_.IsActive()) {
                StartLoadingAnimation();
            }
            else {
                KillTimer(id);
            }
            return;
        }
        spinner_angle_ += spinner::ROTATION_INCREMENT;
        if (spinner_angle_ > TWO_PI) {
            spinner_angle_ -= TWO_PI;
        }
        Invalidate();
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
    case INLINE_MATH_TABLES:
        RelayoutPendingInlineMath(true);
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

void App::ShowToast(std::wstring_view message)
{
    if (message.empty()) {
        return;
    }
    toast_message_ = message;
    SetTimer(app_timer::Id::TOAST, app_timer::TOAST_DURATION_MS);
    Invalidate();
}

void App::ShellOpenAsync(std::wstring target)
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

void App::CancelPendingResources()
{
    mermaid_renderer_.DropQueued();
    image_loader_.DropQueued();
}

void App::StartLoadingAnimation()
{
    spinner_shown_ = true;
    spinner_angle_ = 0.0f;
    SetTimer(app_timer::Id::LOADING_ANIM, app_timer::FRAME_INTERVAL_MS);
    Invalidate();
}

void App::ScheduleLoadingAnimation()
{
    // 回っている最中に次の読み込みが始まったら、そのまま回し続ける。
    if (!spinner_shown_) {
        SetTimer(app_timer::Id::LOADING_ANIM, app_timer::LOADING_SPINNER_DELAY_MS);
    }
}

void App::StopLoadingAnimation()
{
    // スピナーを消すための再描画。後続が描き直さない経路 (同じファイルを開き直して NoChange) もある。
    if (spinner_shown_) {
        Invalidate();
    }
    // 出す前のスピナーの遅延もここで止める。
    KillTimer(app_timer::Id::LOADING_ANIM);
    spinner_shown_ = false;
}

void App::ShowHelpResource()
{
    const auto rc = LoadRcData(i18n::S().help_resource_id);
    if (rc.empty()) {
        return;
    }

    StopLoadingAnimation();
    load_coordinator_.Cancel();

    std::string utf8(reinterpret_cast<const char*>(rc.data()), rc.size());
    Document help = Document::FromMarkdown(std::move(utf8), HELP_PATH);
    LayoutCache estimated = layout::MakeEstimatedLayoutCache(help.GetNodes(), theme_service_.GetTheme(), MdContentWidth(state_));
    OpenDocument(std::move(help), std::move(estimated), /*follow_file_pane=*/false, {});
}

namespace {
// IDWriteTextLayout の解放は 1 個 2〜3µs かかる。
constexpr size_t kDisposeMinLayouts = 256;
} // namespace

void App::DisposeReleasedLayouts()
{
    auto released = state_.document.layout_cache.TakeReleasedLayouts();
    DisposeOnWorkerIfLarge(scheduler_, released.size(), kDisposeMinLayouts, std::move(released));
}

void App::ReplaceDocument(Document next, LayoutCache estimated)
{
    // 以降、doc のノードと layout_cache のエントリは常に同数 (描画・ヒットテスト等はこれを前提にする)。
    assert(estimated.size() == next.GetNodes().size());
    Document old = std::exchange(state_.document.doc, std::move(next));
    LayoutCache old_cache = std::exchange(state_.document.layout_cache, std::move(estimated));
    old_cache.ReleaseDiagrams();
    // 表は 1 ノードにセルの数だけレイアウトを持つので、ノード数によらずワーカーで捨てる。
    scheduler_.Post([o = std::move(old_cache)] {});
    DisposeOnWorkerIfLarge(scheduler_, old.GetNodes().size(), kDisposeMinNodes, std::move(old));
}

void App::BeginAsyncLoad(std::wstring path, bool show_animation)
{
    MENDO_PROFILE("App::BeginAsyncLoad");
    if (show_animation) {
        ScheduleLoadingAnimation();
    }
    ReloadBase reload_base = ReloadBaseFor(state_.document.doc, path, prefix_shrink_retry_.Exhausted());
    // 進行中の読み込み (preload を含む) は置き換える。
    load_coordinator_.Start(scheduler_, std::move(path), theme_service_.GetTheme(), MdContentWidth(state_), std::move(reload_base));
}

void App::DeferReloadRetry()
{
    // FileWatcher は paused のまま維持する。resume すると待機中の変更通知が
    // FILE_RELOAD_DEBOUNCE を 200ms に上書きし、短縮リトライが効かなくなる。
    SetTimer(app_timer::Id::FILE_RELOAD_DEBOUNCE, app_timer::FILE_RELOAD_RETRY_MS);
}

// 読み込みとパースは大きさによらず worker で行い、UI スレッドではファイルに触れない (ネットワーク上のファイルで止まらない)。
void App::LoadMarkdownFile(std::wstring_view path)
{
    MENDO_PROFILE("App::LoadMarkdownFile");
    KillTimer(app_timer::Id::FILE_RELOAD_DEBOUNCE);
    // 止めたリロードや取り消される進行中のリロードは、このロードが取り消し・失敗で終わったときに読み直す。
    file_watcher_.RequeueIfPaused();
    prefix_shrink_retry_.Reset();
    // 仮想パスはファイルとして読めないので、埋め込みリソースから開く。
    if (IsHelpPath(path)) {
        ShowHelpResource();
        return;
    }
    // 旧文書の画像・図の待ちはもう要らない。文書を差し替えずに終わったら、表示を続ける文書のぶんを要求し直す
    // (OnParseComplete・CancelOtherFileLoad)。開いている文書を開き直すとリロードとして扱われ、
    // 図と画像はコードやパスがキーなので待ちをそのまま使える。
    if (!path_util::iequal(path, state_.document.doc.GetFilePath())) {
        CancelPendingResources();
    }
    BeginAsyncLoad(std::wstring{ path }, /*show_animation=*/true);
}

// 同じファイルの再読み込みは FileWatcher を paused にしたまま進んでいるので止めない。
void App::CancelOtherFileLoad()
{
    if (!load_coordinator_.IsActive() ||
        path_util::iequal(load_coordinator_.LoadingPath(), state_.document.doc.GetFilePath())) {
        return;
    }
    StopLoadingAnimation();
    load_coordinator_.Cancel();
    file_watcher_.ResumeWatching();
    resource_manager_.ScheduleBitmapManage();
}

// Init より前に呼ばれるため、推定用のテーマは設定から直接作る。
// 推定に使う寸法はライト/ダーク共通でズームだけに依存するので、ダークモード未読込でも結果は同じ。
void App::StartPreloadAsync(std::wstring path)
{
    load_coordinator_.Start(scheduler_, std::move(path), theme_service_.CreateTheme(session::LoadZoomLevel(config_)));
}

void App::HandleLoadFailureFallback(std::optional<FileLoadError> error)
{
    if (error) {
        ShowToast(FileLoadErrorMessage(*error, i18n::S()));
    }
    // ファイル一覧から開いたファイルが消えていたなら、一覧を今の中身に直す。
    if (error == FileLoadError::NotFound) {
        RefreshFilePane();
    }
    file_watcher_.ResumeWatching();
    Dispatch(LoadFailedAction{});
    if (state_.document.doc.IsEmpty()) {
        ShowHelpResource();
    }
}

void App::OnParseComplete()
{
    MENDO_PROFILE("OnParseComplete");

    auto outcome = load_coordinator_.Take();
    const auto plan = PlanParseComplete(outcome, state_.document.doc);

    if (plan.step == ParseCompleteStep::IgnoreStale) {
        MENDO_TRACE("OnParseComplete: stale message ignored (new load in progress)");
        return;
    }

    StopLoadingAnimation();

    // Fail のときだけ結果が無い。
    AsyncLoadResult* const result = (outcome && *outcome) ? &**outcome : nullptr;

    // 読み始めに捨てた画像・図の待ちを、文書を差し替えずに終わるなら要求し直す。ApplyReload は ReloadDocument が要求する
    // (ドラッグの後に回すときも、読み直しの結果でここか ReloadDocument を通る)。
    if (plan.step != ParseCompleteStep::ReplaceWithResult && plan.step != ParseCompleteStep::ApplyReload) {
        resource_manager_.ScheduleBitmapManage();
    }

    switch (plan.step) {
    case ParseCompleteStep::IgnoreStale:
        break;
    case ParseCompleteStep::Fail:
        MENDO_TRACE("OnParseComplete: no result (cancelled or load failed)");
        HandleLoadFailureFallback(plan.error);
        return;
    case ParseCompleteStep::Retry:
        // worker が作った推定済み LayoutCache は D2D 由来の参照を持たないので、文書ごと worker で捨てられる。
        DisposeOnWorkerIfLarge(scheduler_, result->doc.GetNodes().size(), kDisposeMinNodes, std::move(outcome));
        DeferReloadRetry();
        return;
    case ParseCompleteStep::ReloadUnchanged:
        prefix_shrink_retry_.Reset();
        file_watcher_.ResumeWatching();
        return;
    case ParseCompleteStep::DeferPrefixShrink:
        // 正確な差分を取り直す。上限に達したら、次の読み直しで worker が縮んだ内容を受け入れる (BeginAsyncLoad)。
        prefix_shrink_retry_.CountDefer();
        DeferReloadRetry();
        return;
    case ParseCompleteStep::ApplyReload: {
        const ReloadCheck& reload = *result->reload;
        MENDO_TRACEF("OnParseComplete: reload node_count={} diff_pos={} new_size={}", result->doc.GetNodes().size(),
                     reload.decision.diff_pos, result->doc.GetRawText().size());
        prefix_shrink_retry_.Reset();
        // 読み始めた後にテキスト選択のドラッグが始まった。読み直しはドラッグの後に回す。
        if (DeferReloadWhileSelecting()) {
            DisposeOnWorkerIfLarge(scheduler_, result->doc.GetNodes().size(), kDisposeMinNodes, std::move(outcome));
            return;
        }
        ReloadDocument(std::move(result->doc), std::move(result->cache), reload.ends, reload.decision.diff_pos);
        return;
    }
    case ParseCompleteStep::ReplaceWithResult:
        OpenDocument(std::move(result->doc), std::move(result->cache), plan.follow_file_pane, std::move(result->watch));
        return;
    }
    std::unreachable();
}

void App::OpenDocument(Document next, LayoutCache estimated, bool follow_file_pane, FileWatchTarget watch)
{
    MENDO_PROFILE("OpenDocument");

    ReplaceDocument(std::move(next), std::move(estimated));
    CancelPendingResources();
    // 前の文書の数式を描いた型は、新しい文書ではほとんど使わない。同じファイルの読み直しは組んだレイアウトを
    // 引き継ぐので残す (ReloadDocument)。
    ForgetInlineMath();

    // スクロール復元先の Y がずれないよう、推定高さを Mermaid/画像キャッシュの実測値で補正してから reducer に渡す。
    if (state_.view.pending_restore.IsValid()) {
        MENDO_TRACEF("FinishLoad: node={} offset={}", state_.view.pending_restore.node, state_.view.pending_restore.offset);
        if (ApplyCachedHeights({ 0, state_.document.doc.GetNodes().size() })) {
            layout::RecomputeYPositions(state_.document.doc.GetNodes(), state_.document.layout_cache, theme_service_.GetTheme());
        }
    }
    Dispatch(DocumentOpenedAction{ follow_file_pane });

    RelayoutViewport();

    MENDO_TRACEF("FinishLoad: scroll_y={:.1f} max_scroll={:.1f}", state_.view.viewport.GetScrollY(), MdMaxScroll(state_));

    UpdateTitleBar();
    StartFileWatch(std::move(watch));
    // 開いただけの文書は読むだけかもしれないので、変更の通知で読み直すまでは短く閉じる。
    mermaid_renderer_.SetEditingDiagrams(false);
}

void App::ReloadCurrentFile()
{
    MENDO_PROFILE("ReloadCurrentFile");

    if (!state_.document.doc.HasBackingFile()) {
        return;
    }
    if (DeferReloadWhileSelecting()) {
        return;
    }
    // FileWatcher のバーストで重複スケジュールされないよう、進行中のロードがあれば即 return する。
    if (load_coordinator_.IsActive()) {
        // 別ファイルのロード中なら、それが取り消し・失敗で終わったときの Resume で読み直す。
        file_watcher_.RequeueIfPaused();
        return;
    }
    // ここまでに届いた通知の変更はこれから読む内容に含まれる。残すと Resume で同じ保存をもう一度読み直す。
    file_watcher_.DiscardPendingChange();

    // 編集中の差分リロードでスピナーが点滅すると見づらいため、旧コンテンツを表示したまま差し替える。
    BeginAsyncLoad(state_.document.doc.GetFilePath(), /*show_animation=*/false);
}

// 差し替えると選択の途中でドラッグが終わる。FileWatcher は paused のまま残し、終わるまで短い間隔で確かめ直す。
// LButtonUp の処理中に文書を差し替えないよう、終わった直後も次のタイマーまで待つ。
bool App::DeferReloadWhileSelecting()
{
    if (state_.view.drag.kind != LeftDrag::TextSelect) {
        return false;
    }
    DeferReloadRetry();
    return true;
}

void App::ReloadDocument(Document next, LayoutCache estimated, const UnchangedEnds& ends, size_t diff_pos)
{
    MENDO_PROFILE("ReloadDocument");

    // 保存のたびに可視範囲を作り直さず、実測の高さも推定値に戻さない (スクロール位置の計算と表示の跳ねを防ぐ)。
    const auto carry = layout::CarryOverUnchanged(state_.document.doc.GetNodes(), state_.document.layout_cache, next.GetNodes(), estimated, ends);
    ReplaceDocument(std::move(next), std::move(estimated));
    // 描いた数式は残し、変わった数式を含む組だけが描き直しになる。
    RebuildInlineMathPlan();

    // 図と画像はコードやパスがキーなので、描画・デコード待ちも結果もそのまま使える。
    // 編集で画像が置かれた可能性があるので失敗記録は消す。
    image_loader_.ResetFailedPaths();

    const PaneRect md = ComputePaneLayout(state_).md_rect;

    // Mermaid の図と通常画像の推定高さを実測値 (file_cache_ / image_loader メモリキャッシュ) で
    // 上書きし、CalcScrollYForDiff の Y 計算がずれないようにする。引き継いだノードは実測の高さを持っているので、
    // 当てるのは変わった範囲だけでよい。引き継いだ高さと合わせて Y を 1 回で組み直す。
    const bool applied = ApplyCachedHeights({ carry.ends.head, carry.ends.NewTailBegin() });
    if (applied || carry.carried > 0) {
        layout::RecomputeYPositions(state_.document.doc.GetNodes(), state_.document.layout_cache, theme_service_.GetTheme());
    }

    const float desired_scroll = layout::CalcScrollYForDiff(
        state_.document.doc.GetNodes(), state_.document.layout_cache,
        std::string_view{ state_.document.doc.GetRawText() },
        diff_pos, md.height, state_.view.viewport.GetScrollY());

    MENDO_TRACEF("ReloadDocument: desired_scroll={:.1f} diff_pos={}", desired_scroll, diff_pos);

    // スクロール位置を先に設定してから RelayoutViewport を呼ぶことで、変更箇所周辺の可視ノードが優先的に計測される。
    Dispatch(DocumentReloadedAction{ carry.ends, desired_scroll });

    RelayoutViewport();

    MENDO_TRACEF("ReloadDocument: after-relayout scroll_y={:.1f} max_scroll={:.1f}", state_.view.viewport.GetScrollY(), MdMaxScroll(state_));

    Dispatch(ReloadSettledAction{});

    mermaid_renderer_.SetEditingDiagrams(!state_.document.doc.GetDiagramNodeIndices().empty());
    file_watcher_.ResumeWatching();
}

bool App::ApplyCachedHeights(VisibleRange nodes)
{
    const bool mermaid_applied = resource_manager_.ApplyMermaidCacheHeightsForReload(nodes);
    const bool image_applied = resource_manager_.ApplyCachedImagesForReload(nodes);
    return mermaid_applied || image_applied;
}

void App::StartFileWatch(FileWatchTarget target)
{
    file_watcher_.StartWatching(std::move(target), [this] {
        // 通知の後は読み直すまで監視を止めているので、続けて保存されても最初の通知から数える。
        SetTimer(app_timer::Id::FILE_RELOAD_DEBOUNCE, app_timer::FILE_RELOAD_DEBOUNCE_MS);
    });
}

void App::OpenFileDialog()
{
    if (auto path = file_dialog::OpenMarkdownFile(hwnd_); !path.empty()) {
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
            // ポップアップ (設定ダイアログなど) が持っているキャプチャまで外さない。奪われた通知からも届く。
            if (::GetCapture() == hwnd_) {
                ::ReleaseCapture();
            }
        },
        [this](const effect::ClipboardWrite& ev) { WriteClipboardHtml(hwnd_, ev.html, ev.text); },
        [this](const effect::SetTooltip& ev) {
            tooltip_.SetText(ev.text);
            if (ev.text.empty()) {
                KillTimer(app_timer::Id::TOOLTIP);
            }
            else {
                SetTimer(app_timer::Id::TOOLTIP, app_timer::TOOLTIP_DELAY_MS);
            }
        },
        [this](const effect::ShowContextMenu& ev) {
            OnContextMenu(ev.screen_x, ev.screen_y);
        },
        [](const effect::Beep&) static {
            MessageBeep(MB_OK);
        },
        // ---- Window ----
        [this](const effect::SearchFocus& ev) {
            window_.FocusSearchEdit(ev.anchor, ev.caret);
        },
        [this](const effect::SearchUnfocus& ev) {
            window_.UnfocusSearch(ev.clear_text);
        },
        [this](const effect::ApplyDpi& ev) {
            renderer_.SetDpi(ev.dpi);
            const RECT& r = ev.suggested;
            SetWindowPos(hwnd_, nullptr, r.left, r.top, r.right - r.left, r.bottom - r.top, SWP_NOZORDER | SWP_NOACTIVATE);
        },
        [this](const effect::ApplyZoom&) {
            ApplyZoom();
        },
        [this](const effect::ToggleDarkMode&) {
            ToggleDarkMode();
        },
        [this](const effect::SettleViewport&) {
            SettleViewport();
        },
        [this](const effect::RendererResize& ev) {
            renderer_.Resize(ev.width, ev.height, ev.live);
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
        session::SaveLastFilePath(config_, doc.GetFilePath());
        if (const auto t = SnapshotVisibleTarget(state_); t.IsValid()) {
            session::SaveScrollPosition(config_, t.node, t.offset);
        }
    }

    session::SavePanes(config_, state_.view.panes);
    session::SaveLanguage(config_, i18n::ConfigLangKey());
    // 個別 Save 呼び出しでは write が遅延されるため、終了前に明示 flush で
    // すべての設定値を 1 度のディスク書き込みにまとめる。
    config_.Flush();
}

void App::OnDestroy()
{
    // ログオフ時は OS に打ち切られる前に済ませたいので、スレッドの join より先に保存する。
    SaveSession();

    mermaid_renderer_.Shutdown();
    // 走行中タスクの参照が解放されないよう、
    // LayoutEngine の参照解除 → Shutdown (join) → ターゲット deinit の順を守る。
    measurer_.SetScheduler(nullptr);
    layout_scheduler_.Shutdown();
    // Shutdown はキューを最後まで処理するので、結果を使わないタスクは先に空振りさせる。
    image_loader_.CancelPending();
    load_coordinator_.Cancel();
    scheduler_.Shutdown();
    file_cache_.Shutdown();
    file_cache_.SaveIndex();

    for (UINT_PTR id = std::to_underlying(app_timer::kFirstTimer); id <= std::to_underlying(app_timer::kLastTimer); ++id) {
        ::KillTimer(hwnd_, id);
    }
}

int App::SearchInputPosAt(float dip_x)
{
    const auto sbl = SearchBarLayoutFor(state_, ComputePaneLayout(state_).md_rect);
    return renderer_.HitTestSearchInput(state_.search.query_wide, dip_x - sbl.text_left(), sbl.text_width());
}

RECT App::GetSearchEditRect()
{
    const auto sbl = SearchBarLayoutFor(state_, ComputePaneLayout(state_).md_rect);
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
    const auto pos = session::LoadScrollPosition(config_);
    if (pos.node >= 0) {
        state_.view.pending_restore = { pos.node, static_cast<float>(pos.offset) };
    }
}

void App::ApplyZoom()
{
    state_.document.layout_cache.InvalidateAllLayouts(state_.document.doc.GetNodes());
    theme_service_.ApplyZoom(state_.view.viewport.Zoom());
    measurer_.RecreateFormats();
    renderer_.OnThemeMetricsChanged();
    // 数式は文字の大きさごとに描くので、前の倍率の型は残しても使わない。新しい倍率で描き直させる。
    ForgetInlineMath();
    RelayoutViewport();
    UpdateTitleBar();
    session::SaveZoomLevel(config_, state_.view.viewport.Zoom());
}

void App::ToggleDarkMode()
{
    // 色のみの変更なのでテキストレイアウトは維持する。描画エフェクトの固定ブラシは SetColor で
    // 色だけ差し替わるので再適用不要で、テーマ色を焼き込んだ Mermaid bitmap のみ破棄する。
    state_.document.layout_cache.InvalidateDiagramBitmaps(state_.document.doc.GetDiagramNodeIndices());
    const bool dark = theme_service_.ToggleDarkMode();
    renderer_.OnThemeColorsChanged();
    ApplyDarkModeToWindow(hwnd_, dark);
    tooltip_.ApplyDarkMode(dark);
    // メモリキャッシュはキーにダークモードを含むので消さず、往復時に再利用する。描画待ちは旧テーマの分なので捨てる。
    mermaid_renderer_.DropQueued();
    RelayoutViewport();
    session::SaveDarkMode(config_, dark);
}

App::HitResult App::HitTest(int px, int py)
{
    return hit_test::HitTest(app_controller::MakeMdPaneHitContext(state_, ComputePaneLayout(state_), px, py));
}

// CS_DBLCLKS により連続クリックの 2 回目は WM_LBUTTONDBLCLK で届く。ボタン・スクロールバー・スプリッタは
// 1 回目と同じ判定で連打として扱い、2 回目の扱いが違うもの (本文の単語選択・一覧の項目) は各ゾーンで決める。
void App::OnLButtonDown(int px, int py, bool is_double_click)
{
    const auto dip = PixelToDip(px, py);

    if (HandleTitleBarClick(dip.x, dip.y)) {
        return;
    }

    const auto zone = PaneAtPoint(dip.x);
    switch (zone) {
    case PaneZone::None:
        return;
    case PaneZone::FilePane:
    case PaneZone::TocPane:
        HandleSidePaneClick(*ToPaneTarget(zone), dip.x, dip.y, is_double_click);
        return;
    case PaneZone::Splitter1:
        Dispatch(SplitterDragStartedAction{ PaneTarget::File });
        return;
    case PaneZone::Splitter2:
        Dispatch(SplitterDragStartedAction{ PaneTarget::Toc });
        return;
    case PaneZone::MdPane:
        HandleMdPaneClick(dip.x, dip.y, px, py, is_double_click);
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
    Dispatch(LeftDragEndedAction{ hit.node_index, hit.text_pos });
    if (!small_click || state_.view.viewport.GetSelection().active()) {
        return;
    }
    if (const auto link = app_controller::LinkAtHit(state_, hit)) {
        HandleLinkClick(link->url);
    }
}

void App::OnMouseMove(int px, int py)
{
    const auto dip = PixelToDip(px, py);
    LeftDragMovedAction action{ .dip_x = dip.x, .dip_y = dip.y };
    // 入力欄とテキスト選択だけは、ポインタの位置を文字の位置に直して渡す。
    switch (state_.view.drag.kind) {
    case LeftDrag::None:
        return;
    case LeftDrag::SearchInput:
        action.search_caret = SearchInputPosAt(dip.x);
        break;
    case LeftDrag::TextSelect: {
        const auto hit = HitTest(px, py);
        action.node_index = hit.node_index;
        action.text_pos = hit.text_pos;
        break;
    }
    case LeftDrag::Splitter:
    case LeftDrag::PaneScrollbar:
    case LeftDrag::MdScrollbar:
    case LeftDrag::BlockHScroll:
        break;
    }
    Dispatch(action);
}

void App::OnMouseWheel(int px, int py, int delta, bool ctrl)
{
    const auto dip = PixelToDip(px, py);
    Dispatch(MouseWheelAction{ delta, ctrl, PaneAtPoint(dip.x), GetTickCount64() });
}

bool App::OnRButtonDown(int px, int py)
{
    // 左ドラッグ中の移動は OnMouseMove にしか届かずジェスチャが進まない。離すとメニューが出てドラッグも切れる。
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
    auto result = ClassifyLinkClick(url);
    switch (result.type) {
    case LinkClickResult::Type::None:
        return;
    case LinkClickResult::Type::Anchor:
        Dispatch(NavigateAnchorAction{ std::move(result.target) });
        return;
    case LinkClickResult::Type::ExternalUrl: {
        std::wstring url_wide;
        string_convert::Utf8ToWide(result.target, url_wide);
        ShellOpenAsync(std::move(url_wide));
        return;
    }
    }
    std::unreachable();
}

bool App::HandleTitleBarClick(float dip_x, float dip_y)
{
    if (dip_y >= TitleBar::BASE_HEIGHT) {
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

void App::HandleSearchBarClick(SearchBarHitZone zone, float dip_x, bool is_double_click)
{
    switch (zone) {
    case SearchBarHitZone::None:
        if (!is_double_click) {
            window_.FocusSearchEdit(0, -1);
        }
        return;
    case SearchBarHitZone::Up:
        Dispatch(SearchStepAction{ .forward = false });
        return;
    case SearchBarHitZone::Down:
        Dispatch(SearchStepAction{ .forward = true });
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
        const int pos = SearchInputPosAt(dip_x);
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
            clipboard_manager_.CopyDiagramToClipboard(state_.document.doc, hit.node, png, MdContentWidth(state_), dark);
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
    const auto geom = ResolveBlockHScrollGeometry(state_, hover, hit_ctx.md_rect.width);
    if (!geom.can_scroll() || !hit_test::BlockHScrollbarHitTest(hit_ctx, hover, geom.visible_width)) {
        return false;
    }
    Dispatch(BlockHScrollDragStartedAction{ hover, dip_x });
    return true;
}

void App::HandleMdPaneClick(float dip_x, float dip_y, int px, int py, bool is_double_click)
{
    using Kind = app_controller::MdPaneHit::Kind;
    const auto layout = ComputePaneLayout(state_);
    const auto hit_ctx = app_controller::MakeMdPaneHitContext(state_, layout, px, py);
    const auto target = app_controller::ResolveMdPaneHit(state_, hit_ctx, layout, dip_x, dip_y);
    switch (target.kind) {
    case Kind::SearchBar:
        HandleSearchBarClick(target.search_bar, dip_x, is_double_click);
        return;
    case Kind::NavButton:
        Dispatch(NavigateHistoryAction{ .forward = target.nav == NavDirection::Forward });
        return;
    case Kind::OverlayButton:
        HandleOverlayButtonClick(target.overlay_button);
        return;
    case Kind::Scrollbar:
        Dispatch(MdScrollbarDragStartedAction{ dip_y });
        return;
    case Kind::Content: {
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

void App::HandleSidePaneClick(PaneTarget target, float dip_x, float dip_y, bool is_double_click)
{
    const auto hit = app_controller::ResolveSidePaneHit(state_, target, ComputePaneLayout(state_), dip_x, dip_y);
    if (hit.in_header) {
        switch (hit.button) {
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
    if (hit.scrollbar) {
        Dispatch(PaneScrollbarDragStartedAction{ target, dip_y });
        return;
    }
    // 1 回目のクリックで一覧が変わりうる (フォルダに入る) ので、2 回目を別の項目に当てない。
    if (hit.item < 0 || is_double_click) {
        return;
    }
    if (target == PaneTarget::File) {
        HandleFileEntryClick(state_.file_explorer.GetEntries()[hit.item]);
    }
    else {
        Dispatch(TocItemClickedAction{ state_.document.doc.GetToc()[hit.item].node_index });
    }
}

bool App::IsOverMdScrollbar(float dip_x, float dip_y)
{
    return app_controller::IsOverMdScrollbar(state_, ComputePaneLayout(state_), dip_x, dip_y);
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
    if (SendMessageW(hwnd_, WM_NCHITTEST, 0, MAKELPARAM(pt.x, pt.y)) != HTCLIENT) {
        return;
    }
    ScreenToClient(hwnd_, &pt);
    OnMouseHover(pt.x, pt.y);
}

void App::OnMouseHover(int px, int py)
{
    auto [hover, cursor] = app_controller::ResolvePointerHover(state_, ComputePaneLayout(state_), px, py, IsZoomed(hwnd_));
    SetCursor(cursors_.Get(cursor));
    Dispatch(PointerHoverAction{ std::move(hover) });
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
        .can_edit_file = state_.document.doc.HasBackingFile() && IsEditableTextFile(state_.document.doc.GetFilePath()),
        .has_selection = selection.active() && selection.start_node >= 0,
        .dark_mode_checked = theme_service_.IsDarkMode(),
        .file_pane_checked = panes.IsSidePaneVisible(PaneTarget::File),
        .toc_pane_checked = panes.IsSidePaneVisible(PaneTarget::Toc),
        .show_file_items = zone == PaneZone::MdPane,
        .zoom = &ZoomOf(state_, zoom_pane),
        .on_zoom = [this, zoom_pane](ZoomDirection direction) { Dispatch(ZoomAction{ direction, 1, zoom_pane }); },
    };

    switch (const int cmd = ContextMenu::Show(hwnd_, params); cmd) {
    case IDM_NAV_BACK:
    case IDM_NAV_FORWARD:
        Dispatch(NavigateHistoryAction{ .forward = cmd == IDM_NAV_FORWARD });
        break;
    case IDM_EDIT_FILE: {
        // メニューを開いている間もメッセージは回り、文書が差し替わりうるので確かめ直す。
        const auto& file_path = state_.document.doc.GetFilePath();
        if (IsEditableTextFile(file_path)) {
            ShellOpenAsync(file_path);
        }
        break;
    }
    case IDM_COPY:
    case IDM_COPY_FORMATTED:
        Dispatch(CopyAction{ .formatted = cmd == IDM_COPY_FORMATTED });
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
        .theme = &theme_service_.GetTheme(),
        .dpi_scale = state_.window.cached_dpi_scale,
        .modal_loop = &window_,
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
    renderer_.OnLanguageChanged();
    if (IsHelpPath(state_.document.doc.GetFilePath())) {
        ShowHelpResource();
    }
    Invalidate();
}

void App::ClampScrollToContent()
{
    state_.view.viewport.ClampScroll(MdMaxScroll(state_));
}

bool App::HasIdleWork()
{
    return state_.document.layout_cache.HasDirtyNodes() || state_.search.search_state.IsSearching() || state_.document.layout_cache.HasReleasedLayouts() ||
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
        resource_manager_.OnImagesDecoded(deadline);
        mermaid_renderer_.ProcessDiskLoads(deadline);
        return;
    }
    if (state_.search.search_state.IsSearching()) {
        Dispatch(SearchContinueAction{});
        return;
    }
    if (state_.document.layout_cache.HasDirtyNodes()) {
        OnDeferredLayout();
    }
}

// ロード・ズーム・リサイズでは画面外にもレイアウトが作られ (初回の MeasureTable は全セル)、
// 本文幅が広がると画像の解像度が足りなくなる。スクロールを待たずに BitmapManage で整理させる。
// 遅延レイアウトが残っていれば、その完了時 (OnDeferredLayout) に出す。
void App::SettleLayout()
{
    if (!state_.document.layout_cache.HasDirtyNodes()) {
        resource_manager_.ScheduleBitmapManage();
    }
}

void App::RelayoutViewport()
{
    MENDO_PROFILE("RelayoutViewport");
    const PaneRect md = ComputePaneLayout(state_).md_rect;
    // 可視範囲を即時計測し、scroll_target があれば scroll_y を再評価する。
    layout_service_.ViewportLayout(state_.document.doc, state_.document.layout_cache, md.width, md.height);
    ClampScrollToContent();
    resource_manager_.FlushPendingResources();
    Invalidate();
    SettleLayout();
}

void App::SettleViewport()
{
    MENDO_PROFILE("SettleViewport");

    state_.view.viewport.EnsureScrollTarget(state_.document.layout_cache);
    RelayoutViewport();
}

void App::OnDeferredLayout()
{
    MENDO_PROFILE("OnDeferredLayout");

    // 表やコードブロックが多いと同じノード数でも時間が大きく違うので、件数ではなく時間で区切る。
    // 件数で細かく区切ると、区切るたびに走査・Y の組み直し・ワーカーの待ち合わせが挟まる
    // (test.md x4 の全計測で、32 件ずつだと一度に測る 165ms が 290ms になった)。
    constexpr auto kDeferredLayoutBudget = std::chrono::milliseconds(4);

    const PaneRect md = ComputePaneLayout(state_).md_rect;
    const bool more = layout_service_.ProcessDirtyBatch(
        state_.document.doc, state_.document.layout_cache, md.width, std::chrono::steady_clock::now() + kDeferredLayoutBudget,
        md.height, ResourceManager::EVICT_BUFFER_SCREENS);

    // 中間バッチではクランプしない (ClampScrollToContent の注記)。スクロールバーのドラッグは
    // 移動のたびにその時点の上限で位置を決めるので、ここで合わせる必要はない。
    if (more) {
        return;
    }

    resource_manager_.FlushPendingResources();
    resource_manager_.ScheduleBitmapManage();

    ClampScrollToContent();
    Invalidate();
}

void App::EnsureNodeLayout(int node_index)
{
    layout_service_.EnsureNodeLayout(state_.document.doc, state_.document.layout_cache, ComputePaneLayout(state_).md_rect.width, node_index);
}

void App::RecomputeLayout(layout::HeightChangeRange changed)
{
    layout_service_.RecomputeAfterDiagram(state_.document.doc, state_.document.layout_cache, changed);
    ClampScrollToContent();
    Invalidate();
}
