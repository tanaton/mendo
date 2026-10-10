#include "mermaid.h"
#include "app_constants.h"
#include "d2d_util.h"
#include "file_io.h"
#include "i18n.h"
#include "log_hr.h"
#include "mermaid_file_cache.h"
#include "pmr_format.h"
#include "rc_resource.h"
#include "resource.h"
#include "stream_util.h"
#include "string_convert.h"
#include "task_scheduler.h"
#include "wic_util.h"
#include <wrl/event.h>
#include <algorithm>
#include <cmath>
#include <utility>

#pragma comment(lib, "windowscodecs.lib")

using mendo::LogHrFailure;

namespace {

constexpr std::wstring_view MERMAID_HOST_CLASS = L"mendo_MermaidHost";

constexpr std::wstring_view APP_LOCAL_ORIGIN_PREFIX = L"https://app.local/";
constexpr wchar_t APP_LOCAL_INDEX_URL[] = L"https://app.local/index.html";
// res/mermaid.html の <script src> と一致させる
constexpr std::wstring_view APP_LOCAL_MERMAID_JS_URL = L"https://app.local/mermaid.min.js";

// WebView2 は CapturePreview に IsVisible=TRUE を要求するため、非表示にせず画面外の遠い位置に置く。
constexpr int HOST_OFFSCREEN_POS = -32000;
// どのダイアグラムにも十分な大きさ。
constexpr int HOST_SIZE = 4096;

void DisableWebViewChrome(ICoreWebView2* webview)
{
    Microsoft::WRL::ComPtr<ICoreWebView2Settings> settings;
    LogHrFailure(L"get_Settings", webview->get_Settings(&settings));
    if (!settings) {
        return;
    }
    LogHrFailure(L"put_AreDevToolsEnabled", settings->put_AreDevToolsEnabled(FALSE));
    LogHrFailure(L"put_IsStatusBarEnabled", settings->put_IsStatusBarEnabled(FALSE));
    LogHrFailure(L"put_AreDefaultContextMenusEnabled", settings->put_AreDefaultContextMenusEnabled(FALSE));
    LogHrFailure(L"put_AreDefaultScriptDialogsEnabled", settings->put_AreDefaultScriptDialogsEnabled(FALSE));
}

// CreateStreamOnHGlobal のストリームはアパートメントに縛られないので worker から読める。
// HGLOBAL から直接写し、ゼロ埋めした中間バッファを挟まない。
std::shared_ptr<const std::pmr::vector<uint8_t>> ReadHGlobalStream(IStream* stream)
{
    STATSTG stat{};
    HGLOBAL mem = nullptr;
    if (FAILED(stream->Stat(&stat, STATFLAG_NONAME)) || stat.cbSize.QuadPart == 0 || FAILED(GetHGlobalFromStream(stream, &mem))) {
        return nullptr;
    }
    const auto size = static_cast<size_t>(stat.cbSize.QuadPart);
    if (size > GlobalSize(mem)) {
        return nullptr;
    }
    std::pmr::vector<uint8_t> png;
    png.reserve(size);
    const auto* src = static_cast<const uint8_t*>(GlobalLock(mem));
    if (!src) {
        return nullptr;
    }
    png.assign(src, src + size);
    GlobalUnlock(mem);
    return std::make_shared<const std::pmr::vector<uint8_t>>(std::move(png));
}

} // namespace

MermaidRenderer::~MermaidRenderer()
{
    Shutdown();
}

void MermaidRenderer::Shutdown()
{
    CancelPending();
    latch_.Wait();
    if (hwnd_) {
        KillTimer(hwnd_, std::to_underlying(app_timer::Id::MERMAID_IDLE));
    }
    ReleaseWebView();
}

void MermaidRenderer::ReleaseWebView()
{
    for (auto& w : ActiveWorkers()) {
        DestroyWorker(w);
    }
    worker_count_ = 0;
    webview_env_.Reset();
    lifecycle_.Reset();
}

void MermaidRenderer::Init(
    HWND hwnd, ID2D1RenderTarget* render_target, IWICImagingFactory* wic,
    const std::filesystem::path& user_data_folder, std::move_only_function<void()> on_ready)
{
    hwnd_ = hwnd;
    render_target_ = render_target;
    if (!user_data_folder.empty()) {
        user_data_folder_.assign(user_data_folder.native());
    }
    on_ready_ = std::move(on_ready);

    // PNG デコード用。D2DRenderBackend から共有されなければ自前で作る。
    if (wic) {
        wic_factory_ = wic;
    }
    else {
        wic_factory_ = wic_util::CreateWicFactory(L"MermaidRenderer CoCreateInstance(WIC)");
    }
}

void MermaidRenderer::EnsureInitialized()
{
    if (!lifecycle_.TryMarkInitialized()) {
        return;
    }

    SYSTEM_INFO si{};
    GetSystemInfo(&si);
    target_worker_count_ = mermaid_util::ComputeWorkerCount(si.dwNumberOfProcessors);
    // 最初は 1 つだけ起動し、待ちが溜まったら MaybeGrowWorkers で増やす。
    worker_count_ = CreateWorkerWindow(0) ? 1 : 0;

    if (worker_count_ == 0) {
        // ワーカーウィンドウを 1 つも作れなければ初期化済みフラグを戻し、
        // 次回 RequestRender/RequestSvg で再試行できるようにする。
        // 既にキューされた SVG リクエストは失敗で完了させ in-flight 固着を防ぐ。
        lifecycle_.Reset();
        DrainPendingRequests(/*cancelled=*/false);
        return;
    }

    // WebView2環境の生成失敗はタイマーリトライで対処されるため、
    // ワーカーウィンドウ作成完了時点で（initialized は既にマーク済みで）次段へ進む。
    CreateWebView2Environment();
}

bool MermaidRenderer::CreateWorkerWindow(int index)
{
    static std::once_flag class_register_flag;
    std::call_once(class_register_flag, [] {
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = DefWindowProcW;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = MERMAID_HOST_CLASS.data();
        RegisterClassExW(&wc);
    });

    auto& w = workers_[index];
    w = Worker{};
    w.hwnd = CreateWindowExW(
        WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
        MERMAID_HOST_CLASS.data(),
        L"",
        WS_POPUP,
        HOST_OFFSCREEN_POS, HOST_OFFSCREEN_POS,
        HOST_SIZE, HOST_SIZE,
        nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (!w.hwnd) {
        return false;
    }
    // WebView2 が「可視」と認識するために表示が必要。
    ShowWindow(w.hwnd, SW_SHOWNOACTIVATE);
    return true;
}

MermaidRenderer::Worker* MermaidRenderer::FindIdleWorker() noexcept
{
    const auto workers = ActiveWorkers();
    const auto it = std::ranges::find_if(workers, &Worker::IsIdle);
    return it != workers.end() ? &*it : nullptr;
}

void MermaidRenderer::MaybeGrowWorkers()
{
    if (!webview_env_ || worker_count_ >= target_worker_count_ || pending_requests_.empty()) {
        return;
    }
    if (!std::ranges::all_of(ActiveWorkers(), [](const Worker& w) { return w.ready && w.rendering; })) {
        return;
    }
    const int index = worker_count_;
    if (!CreateWorkerWindow(index)) {
        return;
    }
    ++worker_count_;
    SetupWorker(index);
}

void MermaidRenderer::ScheduleIdleShutdown()
{
    if (!hwnd_ || worker_count_ == 0) {
        return;
    }
    SetTimer(hwnd_, std::to_underlying(app_timer::Id::MERMAID_IDLE), IDLE_SHUTDOWN_MS, nullptr);
}

void MermaidRenderer::OnIdleTimer()
{
    KillTimer(hwnd_, std::to_underlying(app_timer::Id::MERMAID_IDLE));
    if (!pending_requests_.empty()) {
        return;
    }
    // 起動途中や描画中のワーカーがあれば閉じない (非同期ハンドラが閉じたスロットを触らないように)。
    if (!std::ranges::all_of(ActiveWorkers(), &Worker::IsIdle)) {
        return;
    }
    // 次の要求で EnsureInitialized から作り直し、準備完了時の on_ready_ で図を再要求させる。
    ReleaseWebView();
}

void MermaidRenderer::DestroyWorker(Worker& w)
{
    if (w.controller) {
        w.controller->Close();
    }
    if (w.hwnd) {
        DestroyWindow(w.hwnd);
    }
    w = Worker{};
}

void MermaidRenderer::ResizeWorkerView(Worker& worker, int width, int height)
{
    worker.controller->put_Bounds(RECT{ 0, 0, width, height });
    SetWindowPos(worker.hwnd, nullptr, HOST_OFFSCREEN_POS, HOST_OFFSCREEN_POS, width, height, SWP_NOZORDER | SWP_NOACTIVATE);
}

void MermaidRenderer::CreateWebView2Environment()
{
    const wchar_t* user_data = nullptr;
    if (!user_data_folder_.empty()) {
        std::error_code ec;
        std::filesystem::create_directories(user_data_folder_, ec);
        user_data = user_data_folder_.c_str();
    }

    CreateCoreWebView2EnvironmentWithOptions(
        nullptr, user_data, nullptr,
        Microsoft::WRL::Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>(
            [this](HRESULT result, ICoreWebView2Environment* env) -> HRESULT {
                OnEnvironmentCreated(result, env);
                return S_OK;
            }).Get());
}

void MermaidRenderer::OnEnvironmentCreated(HRESULT result, ICoreWebView2Environment* env)
{
    if (FAILED(result) || !env) {
        // 前回プロセスがユーザーデータフォルダをまだ解放していない場合など
        // に失敗する。タイマーで遅延リトライする。
        if (env_retry_count_ < MAX_ENV_RETRIES && hwnd_) {
            ++env_retry_count_;
            SetTimer(hwnd_, std::to_underlying(app_timer::Id::MERMAID_INIT_RETRY), 500, nullptr);
            return;
        }
        // リトライ上限。待機中リクエストを失敗で完了させてから状態を全リセットし、
        // 次回 RequestRender/RequestSvg でクリーンに再初期化させる。リセットしないと
        // initialized_ が立ったままで EnsureInitialized が二度と走らず、以後の
        // リクエストが処理も失敗もされず in-flight 固着する。worker/env を残したまま
        // 再 init するとリークするため Shutdown 経由で破棄する。
        DrainPendingRequests(/*cancelled=*/false);
        env_retry_count_ = 0;
        Shutdown();
        return;
    }
    env_retry_count_ = 0;
    webview_env_ = env;

    for (int i = 0; i < worker_count_; i++) {
        SetupWorker(i);
    }
}

void MermaidRenderer::OnInitRetryTimer()
{
    KillTimer(hwnd_, std::to_underlying(app_timer::Id::MERMAID_INIT_RETRY));
    if (!webview_env_ && worker_count_ > 0) {
        CreateWebView2Environment();
    }
}

void MermaidRenderer::SetupWorker(int index)
{
    webview_env_->CreateCoreWebView2Controller(
        workers_[index].hwnd,
        Microsoft::WRL::Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
            [this, index](HRESULT result, ICoreWebView2Controller* controller) -> HRESULT {
                if (SUCCEEDED(result) && controller) {
                    OnControllerCreated(index, controller);
                }
                return S_OK;
            }).Get());
}

void MermaidRenderer::OnControllerCreated(int index, ICoreWebView2Controller* controller)
{
    auto& w = workers_[index];
    w.controller = controller;
    if (FAILED(controller->get_CoreWebView2(&w.webview)) || !w.webview) {
        w.controller.Reset();
        return;
    }

    controller->put_Bounds(RECT{ 0, 0, HOST_SIZE, HOST_SIZE });
    DisableWebViewChrome(w.webview.Get());
    RegisterWebViewHandlers(index);

    // HTML + JS は WebResourceRequested ハンドラがメモリから配信する。
    LogHrFailure(L"Navigate(initial)", w.webview->Navigate(APP_LOCAL_INDEX_URL));
}

void MermaidRenderer::RegisterWebViewHandlers(int index)
{
    ICoreWebView2* webview = workers_[index].webview.Get();

    LogHrFailure(L"add_WebMessageReceived", webview->add_WebMessageReceived(
        Microsoft::WRL::Callback<ICoreWebView2WebMessageReceivedEventHandler>(
            [this, index](ICoreWebView2*, ICoreWebView2WebMessageReceivedEventArgs* args) -> HRESULT {
                LPWSTR msg = nullptr;
                if (SUCCEEDED(args->TryGetWebMessageAsString(&msg)) && msg) {
                    DispatchWebMessage(index, mermaid_util::ParseWebMessage(msg));
                    CoTaskMemFree(msg);
                }
                return S_OK;
            }).Get(),
        nullptr));

    // レンダラ/ブラウザプロセスのクラッシュを放置するとワーカーが rendering=true の
    // まま恒久的にビジー扱いになり、全ワーカー喪失で以後の図が Loading 固着する。
    LogHrFailure(L"add_ProcessFailed", webview->add_ProcessFailed(
        Microsoft::WRL::Callback<ICoreWebView2ProcessFailedEventHandler>(
            [this, index](ICoreWebView2*, ICoreWebView2ProcessFailedEventArgs*) -> HRESULT {
                RecoverWorker(index);
                return S_OK;
            }).Get(),
        nullptr));

    LogHrFailure(L"add_NavigationStarting", webview->add_NavigationStarting(
        Microsoft::WRL::Callback<ICoreWebView2NavigationStartingEventHandler>(
            [](ICoreWebView2*, ICoreWebView2NavigationStartingEventArgs* args) static -> HRESULT {
                LPWSTR uri = nullptr;
                if (SUCCEEDED(args->get_Uri(&uri)) && uri) {
                    const std::wstring_view u(uri);
                    if (!u.starts_with(APP_LOCAL_ORIGIN_PREFIX) && u != L"about:blank") {
                        args->put_Cancel(TRUE);
                    }
                    CoTaskMemFree(uri);
                }
                return S_OK;
            }).Get(),
        nullptr));

    LogHrFailure(L"add_NewWindowRequested", webview->add_NewWindowRequested(
        Microsoft::WRL::Callback<ICoreWebView2NewWindowRequestedEventHandler>(
            [](ICoreWebView2*, ICoreWebView2NewWindowRequestedEventArgs* args) static -> HRESULT {
                args->put_Handled(TRUE);
                return S_OK;
            }).Get(),
        nullptr));

    // 全URLをフィルタし、app.local 以外へのリクエストもここで遮断する。
    LogHrFailure(L"AddWebResourceRequestedFilter", webview->AddWebResourceRequestedFilter(L"*", COREWEBVIEW2_WEB_RESOURCE_CONTEXT_ALL));
    LogHrFailure(L"add_WebResourceRequested", webview->add_WebResourceRequested(
        Microsoft::WRL::Callback<ICoreWebView2WebResourceRequestedEventHandler>(
            [this](ICoreWebView2*, ICoreWebView2WebResourceRequestedEventArgs* args) -> HRESULT {
                OnWebResourceRequested(args);
                return S_OK;
            }).Get(),
        nullptr));
}

void MermaidRenderer::OnWebResourceRequested(ICoreWebView2WebResourceRequestedEventArgs* args)
{
    Microsoft::WRL::ComPtr<ICoreWebView2WebResourceRequest> request;
    args->get_Request(&request);
    LPWSTR uri = nullptr;
    request->get_Uri(&uri);
    const std::pmr::wstring url(uri ? uri : L"");
    CoTaskMemFree(uri);

    const auto respond = [&](IStream* body, int status, const wchar_t* reason, const wchar_t* response_headers) {
        Microsoft::WRL::ComPtr<ICoreWebView2WebResourceResponse> response;
        webview_env_->CreateWebResourceResponse(body, status, reason, response_headers, &response);
        args->put_Response(response.Get());
    };

    // NavigationStarting と判定を揃え、app.local.evil.com のような部分一致による
    // サブドメイン経由の経路を塞ぐ。
    if (!url.starts_with(APP_LOCAL_ORIGIN_PREFIX)) {
        respond(nullptr, 403, L"Blocked", L"");
        return;
    }

    // その他のパスにはHTMLテンプレート（res/mermaid.html）を配信する
    const bool is_js = url == APP_LOCAL_MERMAID_JS_URL;
    const auto body = LoadRcData(is_js ? IDR_MERMAID_JS : IDR_MERMAID_HTML);
    Microsoft::WRL::ComPtr<IStream> stream;
    if (!body.empty()) {
        stream = stream_util::CreateMemoryStream(body.data(), body.size());
    }

    // リソース欠落時は空ボディを200で返さず500を返して失敗を明示する
    if (!stream) {
        respond(nullptr, 500, L"Resource unavailable", L"");
        return;
    }
    respond(stream.Get(), 200, L"OK", is_js ? L"Content-Type: text/javascript; charset=utf-8" : L"Content-Type: text/html; charset=utf-8");
}

void MermaidRenderer::SetRenderTarget(ID2D1RenderTarget* render_target)
{
    render_target_ = render_target;
    cache_.Clear();
}

void MermaidRenderer::ClearCache()
{
    cache_.Clear();
}

void MermaidRenderer::InsertCache(uint64_t hash, CachedBitmap cached)
{
    cache_.Insert(hash, std::move(cached));
    cache_.TrimToBudget(MAX_CACHE_BYTES, [](const CachedBitmap& c) {
        return (c.png ? c.png->size() : 0) + mendo::BitmapBytes(c.bitmap.Get());
    });
}

void MermaidRenderer::SetDiagramError(RenderRequest& req, std::wstring_view msg)
{
    if (!req.diagram_entry) {
        return;
    }
    // DrawTextCmd の 255 文字上限とプレースホルダ高さ (約3行) に収まる長さ。
    constexpr size_t MAX_ERROR_LEN = 200;
    auto sanitized = mermaid_util::SanitizeErrorMessage(msg, MAX_ERROR_LEN);
    if (sanitized.empty()) {
        sanitized = i18n::S().diagram_error;
    }
    req.diagram_entry->error = std::move(sanitized);
}

void MermaidRenderer::InvokeSvgCallbackIfAny(RenderRequest& req, std::pmr::wstring svg, bool cancelled)
{
    if (auto cb = std::move(req.svg_callback)) {
        cb(std::move(svg), cancelled);
    }
}

void MermaidRenderer::DrainPendingRequests(bool cancelled)
{
    // 待機中の SVG リクエストを完了させ、呼び出し元の in-flight フラグ固着を防ぐ。
    while (!pending_requests_.empty()) {
        auto& req = pending_requests_.front();
        InvokeSvgCallbackIfAny(req, {}, cancelled);
        ReleaseInflight(req);
        pending_requests_.pop();
    }
}

void MermaidRenderer::CancelPending()
{
    // SVG 専用リクエストのコールバックは cancelled=true で呼び、呼び出し元の状態をリセットさせる。
    // PNG レンダリング (on_complete) は無引数のため呼び出し元側でフラグを持っていない前提。
    DrainPendingRequests(true);

    // current_request の request_id が 0 に戻るため、処理中の非同期コールバックは
    // ID 不一致で自動的に無視される。
    // SVG コールバックの再入で worker_count_ が変わり得るため毎回読み直す。
    for (int i = 0; i < worker_count_; i++) {
        auto& w = workers_[i];
        InvokeSvgCallbackIfAny(w.current_request, {}, true);
        w.rendering = false;
        w.current_request = {};
    }
    inflight_entries_.clear();

    disk_gen_.fetch_add(1);
    // 結果の破棄 (bitmap/PNG の解放) を lock 外で行う。
    std::pmr::vector<DiskLoad> stale;
    {
        const std::lock_guard lock(disk_mutex_);
        stale.swap(disk_results_);
    }
}

void MermaidRenderer::DropQueued()
{
    std::queue<RenderRequest, std::pmr::deque<RenderRequest>> kept;
    while (!pending_requests_.empty()) {
        auto& req = pending_requests_.front();
        if (req.svg_only) {
            kept.push(std::move(req));
        }
        else {
            ReleaseInflight(req);
        }
        pending_requests_.pop();
    }
    pending_requests_ = std::move(kept);
}

void MermaidRenderer::RecoverWorker(int index)
{
    auto& w = workers_[index];
    // SVG は失敗完了で呼び出し元の in-flight 固着を防ぐ。PNG は捨てると再リクエストの
    // 契機が無く図が Loading のまま固着するためキューに戻す。クラッシュを誘発する
    // 入力での再起動ループは retried で 1 回に打ち切る。
    InvokeSvgCallbackIfAny(w.current_request, {}, false);
    Callback failed_cb;
    if (w.current_request.node) {
        if (!w.current_request.retried) {
            w.current_request.retried = true;
            pending_requests_.push(std::move(w.current_request));
        }
        else {
            // リトライ済みの再クラッシュ。エラー確定にしないと Loading 固着かつ
            // 完了通知も出ないため、汎用エラーを設定し on_complete で再描画させる。
            SetDiagramError(w.current_request, {});
            failed_cb = std::move(w.current_request.on_complete);
            ReleaseInflight(w.current_request);
        }
    }
    w.current_request = {};
    w.rendering = false;
    w.ready = false;
    // 死んだプロセスの webview/controller は再利用できない
    if (w.controller) {
        w.controller->Close();
    }
    w.controller.Reset();
    w.webview.Reset();
    // init_retries は ready 受信で 0 に戻る。連続クラッシュ時の無限再作成はここで止める。
    if (webview_env_ && w.init_retries < MAX_WORKER_RETRIES) {
        ++w.init_retries;
        SetupWorker(index);
    }
    // ワーカー状態の復旧が済んでから呼び、再入 (recompute → RequestRender) を安全にする。
    if (failed_cb) {
        failed_cb();
    }
}

void MermaidRenderer::ApplyCachedBitmap(NodeLayoutEntry& layout_entry, DiagramEntry& diagram_entry, const CachedBitmap& cached) noexcept
{
    diagram_entry.bitmap = cached.bitmap;
    diagram_entry.width = cached.width;
    diagram_entry.height = cached.height;
    diagram_entry.png = cached.png;
    layout_entry.height = cached.height;
    layout_entry.layout_dirty = false;
}

void MermaidRenderer::RequestRender(
    Node& node, NodeLayoutEntry& layout_entry,
    DiagramEntry& diagram_entry,
    float max_width, bool dark_mode,
    Callback on_complete)
{
    if (!IsDiagramLanguage(node.code_language())) {
        return;
    }

    const auto hash = mermaid_util::NodeDiagramHash(node, max_width, dark_mode);

    if (const auto* cached = cache_.Find(hash)) {
        ApplyCachedBitmap(layout_entry, diagram_entry, *cached);
        if (on_complete) {
            on_complete();
        }
        return;
    }

    if (inflight_entries_.contains(&diagram_entry)) {
        return;
    }

    RenderRequest req{
        .node = &node,
        .layout_entry = &layout_entry,
        .diagram_entry = &diagram_entry,
        .max_width = max_width,
        .dark_mode = dark_mode,
        .on_complete = std::move(on_complete),
        .code_hash = hash,
    };
    if (file_cache_) {
        MermaidFileCache::CacheEntry fentry;
        std::filesystem::path png_path;
        if (file_cache_->LookupPath(hash, fentry, png_path)) {
            req.css_width = fentry.css_width;
            req.css_height = fentry.css_height;
            StartDiskLoad(std::move(req), std::move(png_path));
            return;
        }
    }

    EnqueueWebRender(std::move(req));
}

void MermaidRenderer::StartDiskLoad(RenderRequest req, std::filesystem::path png_path, Microsoft::WRL::ComPtr<IStream> captured_stream)
{
    inflight_entries_.insert(req.diagram_entry);
    const bool captured = captured_stream != nullptr;
    DiskLoad job{ .gen = disk_gen_.load(), .req = std::move(req), .captured_stream = std::move(captured_stream), .captured = captured };
    // ファイル読み込みと PNG デコードは worker で行う (UI では時間予算なしに積み上がる)。
    if (!bg_scheduler_ || !hwnd_ || disk_loaded_msg_ == 0) {
        LoadDiskJob(job, png_path);
        ApplyDiskLoad(job);
        return;
    }
    const DiagramEntry* entry = job.req.diagram_entry;
    const bool posted = bg_scheduler_->Post([this, job = std::move(job), path = std::move(png_path), guard = latch_.Acquire()]() mutable {
        if (disk_gen_.load() != job.gen) {
            return;
        }
        LoadDiskJob(job, path);
        bool was_empty = false;
        {
            const std::lock_guard lock(disk_mutex_);
            if (disk_gen_.load() != job.gen) {
                return;
            }
            was_empty = disk_results_.empty();
            disk_results_.push_back(std::move(job));
        }
        // 未回収の結果があれば通知済みで、受信側がまとめて回収する。
        if (was_empty) {
            ::PostMessageW(hwnd_, disk_loaded_msg_, 0, 0);
        }
    });
    if (!posted) {
        // lambda に move 済みの on_complete は失われるが、図は NeedsRender のまま残り次の走査で再要求される。
        inflight_entries_.erase(entry);
    }
}

void MermaidRenderer::LoadDiskJob(DiskLoad& job, const std::filesystem::path& path)
{
    if (job.captured_stream) {
        job.png = ReadHGlobalStream(job.captured_stream.Get());
        job.captured_stream.Reset();
        if (!job.png) {
            return;
        }
    }
    else {
        auto [data, size] = ReadAllBytes(path, &job.read_error);
        if (!data) {
            return;
        }
        job.png = std::make_shared<const std::pmr::vector<uint8_t>>(data.get(), data.get() + size);
    }
    // job.png はデコード完了まで生きているので、コピーせずに参照するストリームで読む。
    Microsoft::WRL::ComPtr<IWICStream> stream;
    if (FAILED(wic_factory_->CreateStream(&stream)) ||
        FAILED(stream->InitializeFromMemory(const_cast<BYTE*>(job.png->data()), static_cast<DWORD>(job.png->size())))) {
        return;
    }
    if (const auto decoded = wic_util::DecodeFromStream(wic_factory_.Get(), stream.Get())) {
        job.bitmap = wic_util::DecodeToWicBitmap(wic_factory_.Get(), *decoded, { decoded->pixel_width, decoded->pixel_height });
    }
}

void MermaidRenderer::ApplyDiskLoad(DiskLoad& r)
{
    ReleaseInflight(r.req);
    Microsoft::WRL::ComPtr<ID2D1Bitmap> bitmap;
    if (r.bitmap && render_target_ && SUCCEEDED(render_target_->CreateBitmapFromWicBitmap(r.bitmap.Get(), &bitmap)) && bitmap) {
        if (r.captured && file_cache_) {
            file_cache_->StoreAsync(r.req.code_hash, r.req.css_width, r.req.css_height, r.png);
        }
        CachedBitmap cached{ std::move(bitmap), r.req.css_width, r.req.css_height, std::move(r.png) };
        ApplyCachedBitmap(*r.req.layout_entry, *r.req.diagram_entry, cached);
        InsertCache(r.req.code_hash, std::move(cached));
        if (r.req.on_complete) {
            r.req.on_complete();
        }
        return;
    }
    // 描画直後の PNG を描き直しても同じ結果になりうるため、再要求は呼び出し側の走査に任せる。
    if (r.captured) {
        if (r.req.on_complete) {
            r.req.on_complete();
        }
        return;
    }
    if (file_cache_ && !r.png) {
        file_cache_->OnReadFailed(r.req.code_hash, r.read_error);
    }
    EnqueueWebRender(std::move(r.req));
}

void MermaidRenderer::ProcessDiskLoads()
{
    std::pmr::vector<DiskLoad> results;
    {
        const std::lock_guard lock(disk_mutex_);
        results.swap(disk_results_);
    }
    const uint32_t gen = disk_gen_.load();
    for (auto& r : results) {
        if (r.gen == gen) {
            ApplyDiskLoad(r);
        }
    }
}

void MermaidRenderer::EnqueueWebRender(RenderRequest req)
{
    if (!lifecycle_.IsReady()) {
        EnsureInitialized();
        return;
    }

    if (!inflight_entries_.insert(req.diagram_entry).second) {
        return;
    }

    pending_requests_.push(std::move(req));

    ProcessQueue();
}

void MermaidRenderer::RequestSvg(std::wstring_view code, float max_width, bool dark_mode, SvgCallback callback)
{
    RenderRequest req;
    req.svg_only = true;
    req.max_width = max_width;
    req.dark_mode = dark_mode;
    req.svg_callback = std::move(callback);
    req.code_storage.assign(code.begin(), code.end());
    pending_requests_.push(std::move(req));

    if (!lifecycle_.IsReady()) {
        EnsureInitialized();
        return;
    }
    ProcessQueue();
}

void MermaidRenderer::ProcessQueue()
{
    if (!lifecycle_.IsReady()) {
        return;
    }

    // 同一コードの図が複数あるとき、最初の1つのレンダリング完了後に
    // 残りをキャッシュから即座に解決できる。SVG 専用リクエストは PNG ビットマップ
    // キャッシュ対象外なので常にワーカー経由でレンダリングする。
    while (!pending_requests_.empty()) {
        auto& front = pending_requests_.front();
        if (const CachedBitmap* png_hit = front.svg_only ? nullptr : cache_.Find(front.code_hash)) {
            ApplyCachedBitmap(*front.layout_entry, *front.diagram_entry, *png_hit);
            ReleaseInflight(front);
            auto cb = std::move(front.on_complete);
            pending_requests_.pop();
            if (cb) {
                cb();
            }
            continue;
        }

        Worker* idle = FindIdleWorker();
        if (!idle) {
            break; // 全ワーカーがビジー、完了を待つ
        }

        idle->current_request = std::move(front);
        pending_requests_.pop();
        idle->current_request.request_id = ++request_counter_;
        idle->rendering = true;
        RenderInWorker(*idle);
    }
    MaybeGrowWorkers();
    // 待ちが残っている間はアイドル扱いしない。描画中なら OnIdleTimer が見送り、完了時にここで張り直す。
    if (pending_requests_.empty()) {
        ScheduleIdleShutdown();
    }
}

void MermaidRenderer::FinishWorkerRequest(Worker& worker)
{
    worker.rendering = false;
    ReleaseInflight(worker.current_request);
    auto cb = std::move(worker.current_request.on_complete);
    worker.current_request = {};
    if (cb) {
        cb();
    }
    ProcessQueue();
}

void MermaidRenderer::RenderInWorker(Worker& worker)
{
    if (!worker.webview) {
        FinishWorkerRequest(worker);
        return;
    }
    const auto& req = worker.current_request;

    // CSS ビューポートが max_width (DIP) と等しくなるよう境界を物理ピクセルで設定する
    // (WebView2 は内部で devicePixelRatio で除算する)。SVG 出力 (折返し等) も CSS 幅に
    // 依存するため、PNG/SVG 両経路で同じ bounds を使う。
    const int vp_phys = std::max(1, static_cast<int>(std::ceil(req.max_width * worker.dpr)));
    ResizeWorkerView(worker, vp_phys, static_cast<int>(HOST_SIZE * worker.dpr));

    // リクエスト ID を postMessage に含め、C++ 側でコールバックとリクエストを照合する。
    if (req.svg_only) {
        const auto js = PmrFormat(
            L"renderMermaidSvg('{}', {})"
            L".then(function(s){{window.chrome.webview.postMessage('svg-result:{}:'+(s||''));}})"
            L".catch(function(e){{window.chrome.webview.postMessage('render-error:{}:'+String(e));}})",
            mermaid_util::JsEscape(req.code_storage),
            req.dark_mode ? L"true" : L"false",
            req.request_id, req.request_id);
        LogHrFailure(L"ExecuteScript(svg)", worker.webview->ExecuteScript(js.c_str(), nullptr));
        return;
    }

    // WebView2 / JsEscape は wstring 経路のため UTF-8 から変換する。LatexMath は flowchart ラッパに包む。
    const Node& src_node = *req.node;
    std::pmr::wstring code;
    string_convert::Utf8ToWide(src_node.GetText(), code);
    if (src_node.code_language() == SyntaxLanguage::LatexMath) {
        code = mermaid_util::BuildLatexFlowchartCode(code);
    }

    // maxWidth=0 は CSS 制約なし (ビューポートが制約する)。
    const auto js = PmrFormat(
        L"renderMermaid('{}', {}, 0)"
        L".then(function(r){{window.chrome.webview.postMessage('render-result:{}:'+r);}})"
        L".catch(function(e){{window.chrome.webview.postMessage('render-error:{}:'+String(e));}})",
        mermaid_util::JsEscape(code),
        req.dark_mode ? L"true" : L"false",
        req.request_id, req.request_id);
    LogHrFailure(L"ExecuteScript(render)", worker.webview->ExecuteScript(js.c_str(), nullptr));
}

void MermaidRenderer::OnWorkerReady(Worker& worker, float dpr)
{
    if (dpr > 0) {
        worker.dpr = dpr;
    }
    worker.ready = true;
    worker.init_retries = 0;
    // 初期化 (アイドル解放後の再初期化を含む) ごとに、最初のワーカーの準備完了で on_ready_ を呼ぶ。
    // 残りは準備でき次第プールに参加する。
    if (!lifecycle_.IsReady()) {
        lifecycle_.MarkReady();
        if (on_ready_) {
            on_ready_();
        }
    }
    ProcessQueue();
}

void MermaidRenderer::DispatchWebMessage(int index, const mermaid_util::ParsedWebMessage& parsed)
{
    using mermaid_util::WebMessageKind;
    auto& w = workers_[index];
    const auto& req = parsed.request;
    const bool req_id_match = req.valid && req.id == w.current_request.request_id;
    // payload は has_payload のときだけ設定され、それ以外は空 view。
    const std::wstring_view payload = req.payload;
    switch (parsed.kind) {
    case WebMessageKind::Ready:
        OnWorkerReady(w, parsed.ready_dpr);
        return;
    case WebMessageKind::RenderResult:
        if (req_id_match && req.has_payload) {
            OnRenderResult(index, payload);
        }
        return;
    case WebMessageKind::CaptureReady:
        if (req_id_match) {
            DoCapturePreview(index);
        }
        return;
    case WebMessageKind::SvgResult:
        if (req_id_match && w.current_request.svg_only) {
            InvokeSvgCallbackIfAny(w.current_request, std::pmr::wstring{ payload }, false);
            FinishWorkerRequest(w);
        }
        return;
    case WebMessageKind::RenderError:
        if (req_id_match) {
            SetDiagramError(w.current_request, payload);
            InvokeSvgCallbackIfAny(w.current_request, {}, false);
            FinishWorkerRequest(w);
        }
        return;
    case WebMessageKind::Failed:
        // mermaid.jsの読み込みに失敗した場合、ページを再読み込みして再試行する
        if (w.init_retries < MAX_WORKER_RETRIES && w.webview) {
            ++w.init_retries;
            LogHrFailure(L"Navigate(retry)", w.webview->Navigate(APP_LOCAL_INDEX_URL));
        }
        return;
    case WebMessageKind::Unknown:
        return;
    }
}

void MermaidRenderer::OnRenderResult(int worker_idx, std::wstring_view json)
{
    auto& w = workers_[worker_idx];

    // 例: {"ok":true,"width":400,"height":300,"dpr":1.5}。リクエスト ID はメッセージハンドラで照合済み。
    const float dw = mermaid_util::ParseJsonNumber(json, L"\"width\"");
    const float dh = mermaid_util::ParseJsonNumber(json, L"\"height\"");
    float dpr = mermaid_util::ParseJsonNumber(json, L"\"dpr\"");
    if (dpr <= 0) {
        dpr = 1.0f;
    }
    const bool ok = mermaid_util::ParseJsonTrueFlag(json, L"\"ok\"");

    if (!ok || dw <= 0 || dh <= 0) {
        // JS 側 renderMermaid が返す {ok:false, error:...} の内容をプレースホルダに
        // 表示させる。放置すると「読み込み中」のまま固着する (issue #271)。
        SetDiagramError(w.current_request, mermaid_util::ParseJsonString(json, L"\"error\""));
        FinishWorkerRequest(w);
        return;
    }

    // CSS ピクセル寸法を描画サイズ (DIP) として後で使う。
    w.current_request.css_width = dw;
    w.current_request.css_height = dh;

    // キャプチャ用に WebView をダイアグラムの正確な物理ピクセルサイズにリサイズする。
    ResizeWorkerView(w, static_cast<int>(std::ceil(dw * dpr)), static_cast<int>(std::ceil(dh * dpr)));

    // rAF 2 回で新サイズでの再描画を待ってから postMessage で通知する (Promise-await の問題を回避)。
    const auto cap_js = PmrFormat(
        L"requestAnimationFrame(function(){{requestAnimationFrame(function(){{"
        L"window.chrome.webview.postMessage('capture-ready:{}');}});}});",
        w.current_request.request_id);
    LogHrFailure(L"ExecuteScript(capture)", w.webview->ExecuteScript(cap_js.c_str(), nullptr));
}

void MermaidRenderer::DoCapturePreview(int worker_idx)
{
    auto& w = workers_[worker_idx];
    if (!w.webview) {
        FinishWorkerRequest(w);
        return;
    }

    auto png_stream = stream_util::CreateMemoryStream(nullptr, 0);
    if (!png_stream) {
        FinishWorkerRequest(w);
        return;
    }

    // CancelPending 後に到着した古いキャプチャ結果を request_id で弾く。
    const unsigned int req_id = w.current_request.request_id;
    const HRESULT hr = w.webview->CapturePreview(
        COREWEBVIEW2_CAPTURE_PREVIEW_IMAGE_FORMAT_PNG,
        png_stream.Get(),
        Microsoft::WRL::Callback<ICoreWebView2CapturePreviewCompletedHandler>(
            [this, worker_idx, png_stream, req_id](HRESULT capture_hr) -> HRESULT {
                auto& worker = workers_[worker_idx];
                if (worker.current_request.request_id != req_id) {
                    return S_OK;
                }
                if (SUCCEEDED(capture_hr)) {
                    OnCaptureComplete(worker_idx, png_stream);
                }
                else {
                    FinishWorkerRequest(worker);
                }
                return S_OK;
            }).Get());

    if (FAILED(hr)) {
        FinishWorkerRequest(w);
    }
}

void MermaidRenderer::OnCaptureComplete(int worker_idx, Microsoft::WRL::ComPtr<IStream> png_stream)
{
    auto& w = workers_[worker_idx];
    // PNG の読み出しとデコードはディスクキャッシュと同じ経路で worker に任せ、UI スレッドを塞がない。
    // PNG バイト列は bitmap と同じ寿命でメモリ保持し、クリップボードコピーが
    // 非同期/退避され得る file_cache に依存しないようにする。shared で in-memory
    // キャッシュ・DiagramEntry・ディスク書き込みに共有する。
    auto req = std::exchange(w.current_request, {});
    FinishWorkerRequest(w);
    StartDiskLoad(std::move(req), {}, std::move(png_stream));
}
