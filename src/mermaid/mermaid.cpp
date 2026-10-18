#include "mermaid.h"
#include "app_constants.h"
#include "d2d_util.h"
#include "file_io.h"
#include "i18n.h"
#include "mermaid_file_cache.h"
#include "resource.h"
#include "string_convert.h"
#include "task_scheduler.h"
#include "ui_constants.h"
#include "utility.h"
#include "wic_util.h"
#include "win32_util.h"
#include "win_handle.h"
#include <objidl.h>
#include <shellscalingapi.h>
#include <wrl/client.h>
#include <wrl/event.h>
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <utility>

#pragma comment(lib, "shcore.lib")
#pragma comment(lib, "windowscodecs.lib")

namespace {

bool SeekToBegin(IStream* stream) noexcept
{
    const LARGE_INTEGER zero{};
    return SUCCEEDED(stream->Seek(zero, STREAM_SEEK_SET, nullptr));
}

// size == 0 は空ストリームを要求する正当なユースケース（WebView2 CapturePreview の
// 書き込み先バッファ用途）として許容するが、size > 0 && data == nullptr は呼び出し
// 側バグなので nullptr を返して失敗させる。
Microsoft::WRL::ComPtr<IStream> CreateMemoryStream(const void* data, size_t size)
{
    if (size > 0 && !data) {
        return nullptr;
    }
    if (size > std::numeric_limits<ULONG>::max()) {
        return nullptr;
    }

    Microsoft::WRL::ComPtr<IStream> stream;
    if (FAILED(CreateStreamOnHGlobal(nullptr, TRUE, &stream)) || !stream) {
        return nullptr;
    }
    if (size > 0) {
        ULONG written = 0;
        if (FAILED(stream->Write(data, static_cast<ULONG>(size), &written)) || written != size || !SeekToBegin(stream.Get())) {
            return nullptr;
        }
    }
    return stream;
}

constexpr std::wstring_view MERMAID_HOST_CLASS = L"mendo_MermaidHost";

constexpr std::wstring_view APP_LOCAL_ORIGIN_PREFIX = L"https://app.local/";
constexpr wchar_t APP_LOCAL_INDEX_URL[] = L"https://app.local/index.html";
// res/mermaid.html の <script src> と一致させる
constexpr std::wstring_view APP_LOCAL_MERMAID_JS_URL = L"https://app.local/mermaid.min.js";
// mermaid.js に XSS があっても外へ通信させない。WebResourceRequested では WebSocket や Worker 発の
// 通信を捕まえられないので、ページの CSP で止める。eval は mermaid の依存が使いうるので許す。
constexpr wchar_t HTML_RESPONSE_HEADERS[] =
    L"Content-Type: text/html; charset=utf-8\r\n"
    L"Content-Security-Policy: default-src 'none'; script-src https://app.local 'unsafe-inline' 'unsafe-eval'; "
    L"style-src 'unsafe-inline'; img-src data: blob:; font-src data:; worker-src 'none'; base-uri 'none'; form-action 'none'";

// WebView2 は CapturePreview に IsVisible=TRUE を要求するため、非表示にせず画面外の遠い位置に置く。
constexpr int HOST_OFFSCREEN_POS = -32000;
// キャプチャ直前にダイアグラムの実寸へ合わせ直すので、それまでは小さく保つ。WebView2 は
// この大きさの描画面を確保してラスタライズする (4096^2 だと 1 面で 64MB)。
// ダイアグラムの寸法は CSS 幅だけで決まり、ビューポートの高さには依存しない。
constexpr int HOST_SIZE = 256;

void DisableWebViewChrome(ICoreWebView2* webview)
{
    Microsoft::WRL::ComPtr<ICoreWebView2Settings> settings;
    LogHrFailure("get_Settings", webview->get_Settings(&settings));
    if (!settings) {
        return;
    }
    LogHrFailure("put_AreDevToolsEnabled", settings->put_AreDevToolsEnabled(FALSE));
    LogHrFailure("put_IsStatusBarEnabled", settings->put_IsStatusBarEnabled(FALSE));
    LogHrFailure("put_AreDefaultContextMenusEnabled", settings->put_AreDefaultContextMenusEnabled(FALSE));
    LogHrFailure("put_AreDefaultScriptDialogsEnabled", settings->put_AreDefaultScriptDialogsEnabled(FALSE));
    // メモリから配信する固定ページだけを開くので、SmartScreen の評判チェック (URL の外部送信) は要らない。
    if (Microsoft::WRL::ComPtr<ICoreWebView2Settings8> settings8; SUCCEEDED(settings.As(&settings8))) {
        LogHrFailure("put_IsReputationCheckingRequired", settings8->put_IsReputationCheckingRequired(FALSE));
    }
}

// ハンドラ型は add_XXX の引数型から推論する。登録トークンは解除しないので受け取らない。
template <class WebView, class Handler, class Fn>
void AddWebViewHandler(WebView* webview, const char* what, HRESULT (STDMETHODCALLTYPE WebView::*add)(Handler*, EventRegistrationToken*), Fn&& fn)
{
    LogHrFailure(what, (webview->*add)(Microsoft::WRL::Callback<Handler>(std::forward<Fn>(fn)).Get(), nullptr));
}

// CreateStreamOnHGlobal のストリームはアパートメントに縛られないので worker から読める。
// HGLOBAL から直接写し、ゼロ埋めした中間バッファを挟まない。
PngBytes ReadHGlobalStream(IStream* stream)
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
    const auto* src = static_cast<const uint8_t*>(GlobalLock(mem));
    if (!src) {
        return nullptr;
    }
    auto png = std::make_shared<const std::pmr::vector<uint8_t>>(src, src + size);
    GlobalUnlock(mem);
    return png;
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
    HWND hwnd, IWICImagingFactory* wic,
    TaskScheduler& bg_scheduler, UINT disk_loaded_msg,
    const std::filesystem::path& user_data_folder, std::move_only_function<void()> on_update)
{
    hwnd_ = hwnd;
    wic_factory_ = wic;
    bg_scheduler_ = &bg_scheduler;
    disk_loaded_msg_ = disk_loaded_msg;
    if (!user_data_folder.empty()) {
        user_data_folder_.assign(user_data_folder.native());
    }
    on_update_ = std::move(on_update);
}

void MermaidRenderer::EnsureInitialized()
{
    if (!lifecycle_.TryMarkInitialized()) {
        return;
    }

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
    std::call_once(class_register_flag, [] static {
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
        // 自前では何も描かないので、DWM のリダイレクト面 (ウィンドウ面積 x 4B) を確保させない。
        WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_NOREDIRECTIONBITMAP,
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
    if (!webview_env_ || worker_count_ >= MAX_WORKERS || pending_requests_.empty()) {
        return;
    }
    if (!std::ranges::all_of(ActiveWorkers(), [](const Worker& w) static { return w.ready && w.rendering; })) {
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
    // 次の要求で EnsureInitialized から作り直し、準備完了時の on_update_ で図を再要求させる。
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

    // clang-format off
    CreateCoreWebView2EnvironmentWithOptions(
        nullptr,
        user_data,
        nullptr,
        Microsoft::WRL::Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>(
            [this](HRESULT result, ICoreWebView2Environment* env) -> HRESULT {
                OnEnvironmentCreated(result, env);
                return S_OK;
            }
        ).Get()
    );
    // clang-format on
}

void MermaidRenderer::OnEnvironmentCreated(HRESULT result, ICoreWebView2Environment* env)
{
    if (FAILED(result) || !env) {
        // 前回プロセスがユーザーデータフォルダをまだ解放していない場合など
        // に失敗する。タイマーで遅延リトライする。
        if (env_retry_count_ < MAX_ENV_RETRIES && hwnd_) {
            ++env_retry_count_;
            SetTimer(hwnd_, std::to_underlying(app_timer::Id::MERMAID_INIT_RETRY), ENV_RETRY_INTERVAL_MS, nullptr);
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
    // clang-format off
    webview_env_->CreateCoreWebView2Controller(
        workers_[index].hwnd,
        Microsoft::WRL::Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
            [this, index](HRESULT result, ICoreWebView2Controller* controller) -> HRESULT {
                if (SUCCEEDED(result) && controller) {
                    OnControllerCreated(index, controller);
                }
                return S_OK;
            }
        ).Get()
    );
    // clang-format on
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
    LogHrFailure("Navigate(initial)", w.webview->Navigate(APP_LOCAL_INDEX_URL));
}

void MermaidRenderer::RegisterWebViewHandlers(int index)
{
    ICoreWebView2* webview = workers_[index].webview.Get();

    // clang-format off
    AddWebViewHandler(
        webview,
        "add_WebMessageReceived",
        &ICoreWebView2::add_WebMessageReceived,
        [this, index](ICoreWebView2*, ICoreWebView2WebMessageReceivedEventArgs* args) -> HRESULT {
            UniqueCoTaskMemStr msg;
            if (SUCCEEDED(args->TryGetWebMessageAsString(msg.put())) && msg) {
                DispatchWebMessage(index, mermaid_util::ParseWebMessage(msg.get()));
            }
            return S_OK;
        }
    );
    // clang-format on

    // レンダラ/ブラウザプロセスのクラッシュを放置するとワーカーが rendering=true の
    // まま恒久的にビジー扱いになり、全ワーカー喪失で以後の図が Loading 固着する。
    // clang-format off
    AddWebViewHandler(
        webview,
        "add_ProcessFailed",
        &ICoreWebView2::add_ProcessFailed,
        [this, index](ICoreWebView2*, ICoreWebView2ProcessFailedEventArgs*) -> HRESULT {
            RecoverWorker(index);
            return S_OK;
        }
    );
    // clang-format on

    // clang-format off
    AddWebViewHandler(
        webview,
        "add_NavigationStarting",
        &ICoreWebView2::add_NavigationStarting,
        [](ICoreWebView2*, ICoreWebView2NavigationStartingEventArgs* args) static -> HRESULT {
            UniqueCoTaskMemStr uri;
            if (SUCCEEDED(args->get_Uri(uri.put())) && uri) {
                const std::wstring_view u(uri.get());
                if (!u.starts_with(APP_LOCAL_ORIGIN_PREFIX) && u != L"about:blank") {
                    args->put_Cancel(TRUE);
                }
            }
            return S_OK;
        }
    );
    // clang-format on

    // clang-format off
    AddWebViewHandler(
        webview,
        "add_NewWindowRequested",
        &ICoreWebView2::add_NewWindowRequested,
        [](ICoreWebView2*, ICoreWebView2NewWindowRequestedEventArgs* args) static -> HRESULT {
            args->put_Handled(TRUE);
            return S_OK;
        }
    );
    // clang-format on

    // カメラ・位置情報・クリップボード等の権限要求は、図の描画に要らないのですべて拒否する。
    // clang-format off
    AddWebViewHandler(
        webview,
        "add_PermissionRequested",
        &ICoreWebView2::add_PermissionRequested,
        [](ICoreWebView2*, ICoreWebView2PermissionRequestedEventArgs* args) static -> HRESULT {
            args->put_State(COREWEBVIEW2_PERMISSION_STATE_DENY);
            return S_OK;
        }
    );
    // clang-format on

    // blob: などからのダウンロードでファイルを保存させない。
    if (Microsoft::WRL::ComPtr<ICoreWebView2_4> webview4; SUCCEEDED(webview->QueryInterface(IID_PPV_ARGS(&webview4)))) {
        // clang-format off
        AddWebViewHandler(
            webview4.Get(),
            "add_DownloadStarting",
            &ICoreWebView2_4::add_DownloadStarting,
            [](ICoreWebView2*, ICoreWebView2DownloadStartingEventArgs* args) static -> HRESULT {
                args->put_Cancel(TRUE);
                return S_OK;
            }
        );
        // clang-format on
    }

    // 全URLをフィルタし、app.local 以外へのリクエストもここで遮断する。
    // clang-format off
    LogHrFailure("AddWebResourceRequestedFilter", webview->AddWebResourceRequestedFilter(L"*", COREWEBVIEW2_WEB_RESOURCE_CONTEXT_ALL));
    AddWebViewHandler(
        webview,
        "add_WebResourceRequested",
        &ICoreWebView2::add_WebResourceRequested,
        [this](ICoreWebView2*, ICoreWebView2WebResourceRequestedEventArgs* args) -> HRESULT {
            OnWebResourceRequested(args);
            return S_OK;
        }
    );
    // clang-format on
}

void MermaidRenderer::OnWebResourceRequested(ICoreWebView2WebResourceRequestedEventArgs* args)
{
    Microsoft::WRL::ComPtr<ICoreWebView2WebResourceRequest> request;
    args->get_Request(&request);
    std::pmr::wstring url;
    if (UniqueCoTaskMemStr uri; SUCCEEDED(request->get_Uri(uri.put())) && uri) {
        url = uri.get();
    }

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
        stream = CreateMemoryStream(body.data(), body.size());
    }

    // リソース欠落時は空ボディを200で返さず500を返して失敗を明示する
    if (!stream) {
        respond(nullptr, 500, L"Resource unavailable", L"");
        return;
    }
    respond(stream.Get(), 200, L"OK", is_js ? L"Content-Type: text/javascript; charset=utf-8" : HTML_RESPONSE_HEADERS);
}

void MermaidRenderer::SetRenderTarget(ID2D1RenderTarget* render_target)
{
    render_target_ = render_target;
    // ビットマップを作れなかった・大きすぎたなど、旧デバイスで確定したエラーも捨てて再試行させる。
    cache_.Clear();
}

void MermaidRenderer::InsertCache(uint64_t hash, CachedResult result)
{
    cache_.Insert(hash, std::move(result));
    cache_.TrimToBudget(MAX_CACHE_BYTES, [](const CachedResult& c) static {
        return (c.png ? c.png->size() : 0) + BitmapBytes(c.bitmap.Get());
    });
}

void MermaidRenderer::SetDiagramError(const RenderRequest& req, std::wstring_view msg)
{
    if (req.svg_only) {
        return;
    }
    // DrawTextCmd の 255 文字上限とプレースホルダ高さ (約3行) に収まる長さ。
    constexpr size_t MAX_ERROR_LEN = 200;
    auto sanitized = mermaid_util::SanitizeErrorMessage(msg, MAX_ERROR_LEN);
    if (sanitized.empty()) {
        sanitized = i18n::S().diagram_error;
    }
    InsertCache(req.code_hash, CachedResult{ .error = std::move(sanitized) });
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
    inflight_.clear();

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
    bool failed = false;
    if (w.rendering && !w.current_request.svg_only) {
        if (!w.current_request.retried) {
            w.current_request.retried = true;
            pending_requests_.push(std::move(w.current_request));
        }
        else {
            // リトライ済みの再クラッシュ。エラー確定にしないと Loading 固着かつ
            // 完了通知も出ないため、汎用エラーを設定して再描画させる。
            SetDiagramError(w.current_request, {});
            ReleaseInflight(w.current_request);
            failed = true;
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
    if (failed) {
        NotifyUpdate();
    }
}

void MermaidRenderer::ApplyResult(DiagramEntry& diagram_entry, const CachedResult& result)
{
    if (!result.error.empty()) {
        diagram_entry.error = result.error;
        return;
    }
    diagram_entry.bitmap = result.bitmap;
    diagram_entry.width = result.width;
    diagram_entry.height = result.height;
    diagram_entry.png = result.png;
}

bool MermaidRenderer::RequestRender(
    const Node& node,
    DiagramEntry& diagram_entry,
    float max_width, bool dark_mode)
{
    if (node.type != NodeType::Diagram) {
        return false;
    }

    if (diagram_entry.code_hash == 0) {
        diagram_entry.code_hash = mermaid_util::DiagramCodeHash(node);
    }
    const auto hash = mermaid_util::DiagramRequestHash(diagram_entry.code_hash, max_width, dark_mode);

    if (const auto* cached = cache_.Find(hash)) {
        ApplyResult(diagram_entry, *cached);
        return true;
    }
    if (!inflight_.insert(hash).second) {
        return false;
    }

    // ディスクから読めなかったときは描画し直すので、ディスクに当たってもコードを持たせる。
    RenderRequest req{
        .code = std::pmr::string{ node.GetText() },
        .latex = node.code_language() == SyntaxLanguage::LatexMath,
        .max_width = max_width,
        .dark_mode = dark_mode,
        .code_hash = hash,
    };
    if (file_cache_) {
        MermaidFileCache::CacheEntry fentry;
        std::filesystem::path png_path;
        if (file_cache_->LookupPath(hash, fentry, png_path)) {
            req.css_width = fentry.css_width;
            req.css_height = fentry.css_height;
            StartDiskLoad(std::move(req), std::move(png_path));
            return false;
        }
    }

    EnqueueWebRender(std::move(req));
    return false;
}

void MermaidRenderer::StartDiskLoad(RenderRequest req, std::filesystem::path png_path, Microsoft::WRL::ComPtr<IStream> captured_stream)
{
    const bool captured = captured_stream != nullptr;
    const uint64_t hash = req.code_hash;
    DiskLoad job{ .req = std::move(req), .captured_stream = std::move(captured_stream), .captured = captured };
    // ファイル読み込みと PNG デコードは worker で行う (UI では時間予算なしに積み上がる)。
    const bool posted = bg_scheduler_->Post([this, job = std::move(job), path = std::move(png_path), guard = latch_.Acquire()]() mutable {
        LoadDiskJob(job, path);
        bool was_empty = false;
        {
            const std::lock_guard lock(disk_mutex_);
            was_empty = disk_results_.empty();
            disk_results_.push_back(std::move(job));
        }
        // 未回収の結果があれば通知済みで、受信側がまとめて回収する。
        if (was_empty) {
            ::PostMessageW(hwnd_, disk_loaded_msg_, 0, 0);
        }
    });
    if (!posted) {
        // 図は NeedsRender のまま残り次の走査で再要求される。
        inflight_.erase(hash);
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
        auto data = ReadAllBytes(path.c_str(), &job.read_error);
        if (data.empty()) {
            return;
        }
        job.png = std::make_shared<const std::pmr::vector<uint8_t>>(std::move(data));
    }
    TaskScheduler::EnsureWorkerCom();
    // job.png はデコード完了まで生きているので、コピーせずに参照して読む。
    if (const auto decoded = wic_util::DecodeFromMemory(wic_factory_.Get(), *job.png, GUID_ContainerFormatPng)) {
        job.bitmap = wic_util::DecodeToWicBitmap(wic_factory_.Get(), *decoded, { decoded->pixel_width, decoded->pixel_height });
    }
}

bool MermaidRenderer::ApplyDiskLoad(DiskLoad& r)
{
    Microsoft::WRL::ComPtr<ID2D1Bitmap> bitmap;
    if (r.bitmap && render_target_ && SUCCEEDED(render_target_->CreateBitmapFromWicBitmap(r.bitmap.Get(), &bitmap)) && bitmap) {
        if (r.captured && file_cache_) {
            file_cache_->StoreAsync(r.req.code_hash, r.req.css_width, r.req.css_height, r.png);
        }
        InsertCache(r.req.code_hash, CachedResult{ std::move(bitmap), r.req.css_width, r.req.css_height, std::move(r.png) });
        ReleaseInflight(r.req);
        return true;
    }
    // 描画直後の PNG を描き直しても同じ結果になるので、エラーを確定させて再要求させない。
    if (r.captured) {
        SetDiagramError(r.req, {});
        ReleaseInflight(r.req);
        return true;
    }
    if (file_cache_ && !r.png) {
        file_cache_->OnReadFailed(r.req.code_hash, r.read_error);
    }
    EnqueueWebRender(std::move(r.req));
    return false;
}

void MermaidRenderer::ProcessDiskLoads()
{
    std::pmr::vector<DiskLoad> results;
    {
        const std::lock_guard lock(disk_mutex_);
        results.swap(disk_results_);
    }
    bool resolved = false;
    for (auto& r : results) {
        resolved |= ApplyDiskLoad(r);
    }
    // まとめて 1 回だけ要求し直させる。
    if (resolved) {
        NotifyUpdate();
    }
}

void MermaidRenderer::EnqueueWebRender(RenderRequest req)
{
    if (!lifecycle_.IsReady()) {
        // 準備完了の通知で要求し直させる。
        ReleaseInflight(req);
        EnsureInitialized();
        return;
    }

    pending_requests_.push(std::move(req));

    ProcessQueue();
}

void MermaidRenderer::RequestSvg(std::string_view code, float max_width, bool dark_mode, SvgCallback callback)
{
    RenderRequest req;
    req.svg_only = true;
    req.max_width = max_width;
    req.dark_mode = dark_mode;
    req.svg_callback = std::move(callback);
    req.code.assign(code);
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
        // 結果を出した要求が通知済みなので、ここでは取り除くだけでよい。
        if (!front.svg_only && cache_.Contains(front.code_hash)) {
            ReleaseInflight(front);
            pending_requests_.pop();
            continue;
        }

        Worker* idle = FindIdleWorker();
        if (!idle) {
            break;
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
    const bool resolved = !worker.current_request.svg_only && cache_.Contains(worker.current_request.code_hash);
    worker.rendering = false;
    ReleaseInflight(worker.current_request);
    worker.current_request = {};
    ProcessQueue();
    if (resolved) {
        NotifyUpdate();
    }
}

void MermaidRenderer::RenderInWorker(Worker& worker)
{
    if (!worker.webview) {
        FinishWorkerRequest(worker);
        return;
    }
    const auto& req = worker.current_request;

    // WebView2 / JsEscape は wstring 経路のため、ここで UTF-8 から変換する。
    std::pmr::wstring code;
    string_convert::Utf8ToWide(req.code, code);
    if (req.latex) {
        code = mermaid_util::BuildLatexFlowchartCode(code);
    }

    // CSS ビューポートが max_width (DIP) と等しくなるよう境界を物理ピクセルで設定する
    // (WebView2 は内部で devicePixelRatio で除算する)。SVG 出力 (折返し等) も CSS 幅に
    // 依存するため、PNG/SVG 両経路で同じ bounds を使う。
    const int vp_phys = std::max(1, static_cast<int>(std::ceil(req.max_width * worker.dpr)));
    ResizeWorkerView(worker, vp_phys, HOST_SIZE);

    // リクエスト ID を postMessage に含め、C++ 側でコールバックとリクエストを照合する。
    if (req.svg_only) {
        const auto js = PmrFormat(
            L"renderMermaidSvg('{}', {})"
            L".then(function(s){{window.chrome.webview.postMessage('svg-result:{}:'+(s||''));}})"
            L".catch(function(e){{window.chrome.webview.postMessage('render-error:{}:'+String(e));}})",
            mermaid_util::JsEscape(code),
            req.dark_mode ? L"true" : L"false",
            req.request_id, req.request_id);
        LogHrFailure("ExecuteScript(svg)", worker.webview->ExecuteScript(js.c_str(), nullptr));
        return;
    }

    // maxWidth=0 は CSS 制約なし (ビューポートが制約する)。
    const auto js = PmrFormat(
        L"renderMermaid('{}', {}, 0)"
        L".then(function(r){{window.chrome.webview.postMessage('render-result:{}:'+r);}})"
        L".catch(function(e){{window.chrome.webview.postMessage('render-error:{}:'+String(e));}})",
        mermaid_util::JsEscape(code),
        req.dark_mode ? L"true" : L"false",
        req.request_id, req.request_id);
    LogHrFailure("ExecuteScript(render)", worker.webview->ExecuteScript(js.c_str(), nullptr));
}

float MermaidRenderer::EstimateWorkerDpr() noexcept
{
    const HMONITOR monitor = MonitorFromPoint(POINT{ HOST_OFFSCREEN_POS, HOST_OFFSCREEN_POS }, MONITOR_DEFAULTTONEAREST);
    UINT dpi_x = 0;
    UINT dpi_y = 0;
    if (FAILED(GetDpiForMonitor(monitor, MDT_EFFECTIVE_DPI, &dpi_x, &dpi_y))) {
        dpi_x = 0;
    }
    return DpiScaleFrom(static_cast<float>(dpi_x));
}

void MermaidRenderer::OnWorkerReady(Worker& worker, float dpr)
{
    if (mermaid_util::IsPlausibleDpr(dpr)) {
        worker.dpr = dpr;
        // 拡大率の変更などで推定とずれていたら、以降のキャッシュは実際に描く DPR で引く。
        if (file_cache_) {
            file_cache_->SetDpr(dpr);
        }
    }
    worker.ready = true;
    worker.init_retries = 0;
    // 初期化 (アイドル解放後の再初期化を含む) ごとに、最初のワーカーの準備完了で on_update_ を呼ぶ。
    // 残りは準備でき次第プールに参加する。
    if (!lifecycle_.IsReady()) {
        lifecycle_.MarkReady();
        NotifyUpdate();
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
    case WebMessageKind::Unknown:
        return;
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
            LogHrFailure("Navigate(retry)", w.webview->Navigate(APP_LOCAL_INDEX_URL));
        }
        return;
    }
    std::unreachable();
}

void MermaidRenderer::OnRenderResult(int worker_idx, std::wstring_view json)
{
    auto& w = workers_[worker_idx];

    // 例: {"ok":true,"width":400,"height":300,"dpr":1.5}。リクエスト ID はメッセージハンドラで照合済み。
    const float dw = mermaid_util::ParseJsonNumber(json, L"\"width\"");
    const float dh = mermaid_util::ParseJsonNumber(json, L"\"height\"");
    float dpr = mermaid_util::ParseJsonNumber(json, L"\"dpr\"");
    if (!mermaid_util::IsPlausibleDpr(dpr)) {
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

    // 描けない大きさのままキャプチャすると、毎回ビットマップ作成に失敗して描き直しを繰り返す。
    if (!mermaid_util::FitsCaptureLimit(dw, dh, dpr, MaxBitmapDim(render_target_))) {
        SetDiagramError(w.current_request, i18n::S().diagram_too_large);
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
    LogHrFailure("ExecuteScript(capture)", w.webview->ExecuteScript(cap_js.c_str(), nullptr));
}

void MermaidRenderer::DoCapturePreview(int worker_idx)
{
    auto& w = workers_[worker_idx];
    if (!w.webview) {
        FinishWorkerRequest(w);
        return;
    }

    auto png_stream = CreateMemoryStream(nullptr, 0);
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
    // 読み出しが終わるまで要求は inflight_ に残る (current_request を空にしてから終えるので外れない)。
    auto req = std::exchange(w.current_request, {});
    FinishWorkerRequest(w);
    StartDiskLoad(std::move(req), {}, std::move(png_stream));
}
