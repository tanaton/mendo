#include "mermaid.h"
#include "app_constants.h"
#include "d2d_util.h"
#include "file_io.h"
#include "katex_chunk.h"
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
#include <WebView2EnvironmentOptions.h>
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <utility>

namespace {

bool SeekToBegin(IStream* stream) noexcept
{
    const LARGE_INTEGER zero{};
    return SUCCEEDED(stream->Seek(zero, STREAM_SEEK_SET, nullptr));
}

// 空なら書き込み先 (CapturePreview) に使う空のストリーム。読み出し (ReadHGlobalStream) が HGLOBAL を前提にするので、
// どちらも CreateStreamOnHGlobal で作る。
Microsoft::WRL::ComPtr<IStream> CreateMemoryStream(std::span<const std::byte> body = {})
{
    if (body.size() > std::numeric_limits<ULONG>::max()) {
        return nullptr;
    }
    Microsoft::WRL::ComPtr<IStream> stream;
    if (FAILED(CreateStreamOnHGlobal(nullptr, TRUE, &stream)) || !stream) {
        return nullptr;
    }
    if (!body.empty()) {
        ULONG written = 0;
        if (FAILED(stream->Write(body.data(), static_cast<ULONG>(body.size()), &written)) || written != body.size() || !SeekToBegin(stream.Get())) {
            return nullptr;
        }
    }
    return stream;
}

constexpr std::wstring_view MERMAID_HOST_CLASS = L"mendo_MermaidHost";

constexpr std::wstring_view APP_LOCAL_ORIGIN_PREFIX = L"https://app.local/";
constexpr wchar_t APP_LOCAL_INDEX_URL[] = L"https://app.local/index.html";
// mermaid.html が KaTeX を読む名前。チャンクの相対 import が解決するよう、実物と同じフォルダに置く。
constexpr std::wstring_view KATEX_ALIAS_PATH = L"chunks/mermaid.esm.min/katex.mjs";
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

// app.local の外への遷移・新しいウィンドウ・権限要求・ダウンロードを止める。
// 外部への通信そのものは WebResourceRequested と CSP で止める。
void RestrictWebView(ICoreWebView2* webview)
{
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
    auto png = std::make_shared<const ByteBuffer>(std::span{ src, size });
    GlobalUnlock(mem);
    return png;
}

// PNG を原寸で読み、このスレッドで画素を確定させた IWICBitmap を作る。失敗したら null。
Microsoft::WRL::ComPtr<IWICBitmap> DecodePngToWicBitmap(IWICImagingFactory* wic, const ByteBuffer& png)
{
    const auto decoded = wic_util::DecodeFromMemory(wic, png, GUID_ContainerFormatPng);
    return decoded ? wic_util::DecodeToWicBitmap(wic, *decoded, { decoded->pixel_width, decoded->pixel_height }) : nullptr;
}

// source を 32bpp BGRA の PNG にする。失敗したら null。
PngBytes EncodePng(IWICImagingFactory* wic, IWICBitmapSource* source)
{
    const auto bgra = wic_util::ConvertBitmapSource(wic, source, GUID_WICPixelFormat32bppBGRA);
    const auto stream = CreateMemoryStream();
    Microsoft::WRL::ComPtr<IWICBitmapEncoder> encoder;
    Microsoft::WRL::ComPtr<IWICBitmapFrameEncode> frame;
    UINT width = 0;
    UINT height = 0;
    WICPixelFormatGUID format = GUID_WICPixelFormat32bppBGRA;
    if (!bgra || !stream || FAILED(bgra->GetSize(&width, &height)) || FAILED(wic->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder)) ||
        FAILED(encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache)) || FAILED(encoder->CreateNewFrame(&frame, nullptr)) ||
        FAILED(frame->Initialize(nullptr)) || FAILED(frame->SetSize(width, height)) || FAILED(frame->SetPixelFormat(&format)) ||
        format != GUID_WICPixelFormat32bppBGRA || FAILED(frame->WriteSource(bgra.Get(), nullptr)) || FAILED(frame->Commit()) ||
        FAILED(encoder->Commit())) {
        return nullptr;
    }
    return ReadHGlobalStream(stream.Get());
}

// ページを撮るビューの大きさ (物理 px)。CSS px を切り上げる。
WICRect PageRect(const mermaid_util::RenderSize& size) noexcept
{
    return { 0, 0, DipToPixelCeil(size.width, size.dpr), DipToPixelCeil(size.height, size.dpr) };
}

// 左上を原点とする rect を width x height (物理 px) の内側に切り詰める。切り出せなければ nullopt。
// WebView2 は dpr 1.5 などでビューの画素数が CSS px で割り切れないとき、ビューより 1px 広く撮り、背景が右と下に
// 1 列余る。描いたものの大きさちょうどに切り出し、描くときに余りの分だけ縮めない (重なる部分は画素一致する)。
std::optional<WICRect> ClipToCapture(WICRect rect, UINT width, UINT height) noexcept
{
    rect.Width = std::min(rect.Width, static_cast<INT>(width));
    rect.Height = std::min(rect.Height, static_cast<INT>(height));
    if (rect.Width <= 0 || rect.Height <= 0) {
        return std::nullopt;
    }
    return rect;
}

// キャプチャ png を rect に切り出し、このスレッドで画素を確定させたビットマップとその PNG にする。rect がキャプチャ全体なら
// エンコードし直さずに png を使う。切り出せなければビットマップは null。
// 図 27 個の文書で描いた 7 個のうち切り出しが要るのは 1〜2 個で、1 回約 4ms (worker)。開いてから描き終えるまでは
// 1408 → 1436ms (2 個切り出したとき。5 巡交互の中央値、ばらつき 1254〜1459ms の内)。
// 切り出すときはデコーダから rect だけを確定させる。全体を確定させてから写すのと比べて 1 回 4.34 → 4.25ms
// (5 巡交互の中央値。大半はエンコード)。
std::pair<Microsoft::WRL::ComPtr<IWICBitmap>, PngBytes> DecodeCapture(IWICImagingFactory* wic, const PngBytes& png, const WICRect& rect)
{
    const auto decoded = wic_util::DecodeFromMemory(wic, *png, GUID_ContainerFormatPng);
    if (!decoded) {
        return {};
    }
    const UINT width = decoded->pixel_width;
    const UINT height = decoded->pixel_height;
    if (std::cmp_greater_equal(rect.Width, width) && std::cmp_greater_equal(rect.Height, height)) {
        return { wic_util::DecodeToWicBitmap(wic, *decoded, { width, height }), png };
    }
    const auto clip = ClipToCapture(rect, width, height);
    Microsoft::WRL::ComPtr<IWICBitmapClipper> clipper;
    Microsoft::WRL::ComPtr<IWICBitmap> bitmap;
    if (!clip || FAILED(wic->CreateBitmapClipper(&clipper)) || FAILED(clipper->Initialize(decoded->converter.Get(), &*clip)) ||
        FAILED(wic->CreateBitmapFromSource(clipper.Get(), WICBitmapCacheOnLoad, &bitmap))) {
        return {};
    }
    auto encoded = EncodePng(wic, bitmap.Get());
    return { std::move(bitmap), std::move(encoded) };
}

// 画面外でも表示中のトップレベルなので、外部ツールが本体の代わりにこれを閉じに来る (CloseMainWindow など)。
// 自分は破棄されず、閉じる要求は本体へ回す。
LRESULT CALLBACK HostWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    const HWND main = BindWindowInstance<HWND__>(hwnd, msg, lParam);
    if (msg == WM_CLOSE) {
        if (main) {
            PostMessageW(main, WM_CLOSE, 0, 0);
        }
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
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
    ready_ = false;
}

void MermaidRenderer::Init(
    HWND hwnd, IWICImagingFactory* wic, MermaidFileCache& file_cache,
    TaskScheduler& bg_scheduler,
    const std::filesystem::path& user_data_folder, std::move_only_function<void()> on_update)
{
    hwnd_ = hwnd;
    file_cache_ = &file_cache;
    wic_factory_ = wic;
    bg_scheduler_ = &bg_scheduler;
    if (!user_data_folder.empty()) {
        user_data_folder_.assign(user_data_folder.native());
    }
    on_update_ = std::move(on_update);
}

// WebView2 は図の描画要求が来てから起動し、ファイルの変更通知などで先に起動しない
// (57621be4 で入れ、f181f428 で外した)。パースと描画が速く先に起動しても体感の差が出ず、
// 図の無い文書の方が多いので、起動すれば数十 MB 常駐するプロセスを要るまで作らない。
void MermaidRenderer::EnsureInitialized()
{
    if (worker_count_ > 0) {
        return;
    }

    // 最初は 1 つだけ起動し、待ちが溜まったら MaybeGrowWorkers で増やす。
    worker_count_ = CreateWorkerWindow(0) ? 1 : 0;

    if (worker_count_ == 0) {
        // ワーカーウィンドウを 1 つも作れなければ、次回 RequestRender/RequestSvg で再試行する。
        // 既にキューされた SVG リクエストは失敗で完了させ in-flight 固着を防ぐ。
        DrainPendingRequests(/*cancelled=*/false);
        return;
    }

    // WebView2環境の生成失敗はタイマーリトライで対処されるため、
    // ワーカーウィンドウ作成完了時点で次段へ進む。
    CreateWebView2Environment();
}

bool MermaidRenderer::CreateWorkerWindow(int index)
{
    static const bool registered = [] {
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = HostWndProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = MERMAID_HOST_CLASS.data();
        return RegisterClassExW(&wc) != 0;
    }();
    if (!registered) {
        return false;
    }

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
        nullptr, nullptr, GetModuleHandleW(nullptr), hwnd_);
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
    if (!std::ranges::all_of(ActiveWorkers(), [](const Worker& w) static { return w.ready && w.IsRendering(); })) {
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
    SetTimer(hwnd_, std::to_underlying(app_timer::Id::MERMAID_IDLE), editing_diagrams_ ? IDLE_SHUTDOWN_EDITING_MS : IDLE_SHUTDOWN_MS, nullptr);
}

void MermaidRenderer::SetEditingDiagrams(bool editing)
{
    if (std::exchange(editing_diagrams_, editing) != editing) {
        // 張り済みの待ち時間を新しい長さで張り直す。描画中なら OnIdleTimer が見送り、完了時に張り直される。
        ScheduleIdleShutdown();
    }
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

    // 図は SVG 1 枚をオフスクリーンで描いて写すだけなので GPU プロセスは要らない。止めると mendo + WebView2 の
    // commit が 3 割強減り (シーケンス図 1 つで 262MB → 171MB)、描き終わるまでも 1〜2 割縮む。写した図は GPU ありと同じ。
    const auto options = Microsoft::WRL::Make<CoreWebView2EnvironmentOptions>();
    options->put_AdditionalBrowserArguments(L"--disable-gpu");

    // clang-format off
    CreateCoreWebView2EnvironmentWithOptions(
        nullptr,
        user_data,
        options.Get(),
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
        // リトライ上限。待機中リクエストを失敗で完了させてからワーカーを破棄し、
        // 次回 RequestRender/RequestSvg でクリーンに再初期化させる。破棄しないと
        // worker_count_ > 0 のままで EnsureInitialized が二度と走らず、以後の
        // リクエストが処理も失敗もされず in-flight 固着する。
        // Shutdown は使わない。関係のないディスクキャッシュの読み込みまで取り消し、共有 scheduler の完了待ちで UI を止める。
        DrainPendingRequests(/*cancelled=*/false);
        env_retry_count_ = 0;
        ReleaseWebView();
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
    // インライン数式の型をアルファ付きでキャプチャするため。図はページの body が背景を塗る。
    if (Microsoft::WRL::ComPtr<ICoreWebView2Controller2> controller2; SUCCEEDED(controller->QueryInterface(IID_PPV_ARGS(&controller2)))) {
        LogHrFailure("put_DefaultBackgroundColor", controller2->put_DefaultBackgroundColor(COREWEBVIEW2_COLOR{ 0, 0, 0, 0 }));
    }
    DisableWebViewChrome(w.webview.Get());
    RestrictWebView(w.webview.Get());
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

    // レンダラ/ブラウザプロセスのクラッシュを放置するとワーカーが描画中 (IsRendering) の
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
    UniqueCoTaskMemStr uri;
    const std::wstring_view url = SUCCEEDED(request->get_Uri(uri.put())) && uri ? std::wstring_view{ uri.get() } : std::wstring_view{};

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

    // mermaid の ESM ファイルは third_party/mermaid からの相対パスを名前にして埋め込んでいる。
    // 名前で引けるのはそれだけなので、数値 ID の他のリソースは返らない。path は NUL 終端した uri の末尾。
    const bool is_html = url == APP_LOCAL_INDEX_URL;
    const std::wstring_view path = url.substr(APP_LOCAL_ORIGIN_PREFIX.size());
    const auto body = is_html                            ? LoadRcData(IDR_MERMAID_HTML)
                      : path == KATEX_ALIAS_PATH         ? LoadRcData(KATEX_CHUNK_RESOURCE)
                      : mermaid_util::IsModulePath(path) ? LoadRcData(path.data())
                                                         : std::span<const std::byte>{};
    Microsoft::WRL::ComPtr<IStream> stream;
    if (!body.empty()) {
        stream = CreateMemoryStream(body);
    }

    // 空ボディを200で返さず、失敗を明示する
    if (!stream) {
        respond(nullptr, 404, L"Not Found", L"");
        return;
    }
    respond(stream.Get(), 200, L"OK", is_html ? HTML_RESPONSE_HEADERS : L"Content-Type: text/javascript; charset=utf-8");
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

// 幅を混ぜないキーの結果は、いまの幅の上限より狭いときだけ使う。
const MermaidRenderer::CachedResult* MermaidRenderer::FindCached(uint64_t code_hash, uint64_t width_free_hash, float max_width)
{
    if (const auto* free = cache_.Find(width_free_hash); free && mermaid_util::IsWidthFree(free->width, max_width)) {
        return free;
    }
    return cache_.Find(code_hash);
}

void MermaidRenderer::SetDiagramError(const RenderRequest& req, DiagramError::Kind kind, std::wstring_view msg)
{
    if (!req.is_display()) {
        return;
    }
    // 枠に収まらない分は描くときに省略記号で切る (placeholder_text の書式)。ページの文言は長さに上限が無いので、
    // キャッシュと TextLayoutCache のキーに丸ごと持つ量だけをここで抑える。
    constexpr size_t MAX_ERROR_LEN = 1024;
    InsertCache(req.code_hash, CachedResult{
                                   .error = { kind, mermaid_util::SanitizeErrorMessage(msg, MAX_ERROR_LEN) }
    });
}

void MermaidRenderer::InvokeSvgCallbackIfAny(RenderRequest& req, std::wstring svg, bool cancelled)
{
    if (auto cb = std::move(req.svg_callback)) {
        cb(std::move(svg), cancelled);
    }
}

void MermaidRenderer::FailMathCallbackIfAny(RenderRequest& req, bool cancelled)
{
    if (auto cb = std::move(req.math_callback)) {
        InlineMathBatchResult result{ .reserved_bytes = req.math_reserved_bytes, .failed = !cancelled, .cancelled = cancelled };
        result.items.reserve(req.math_items.size());
        for (const auto& item : req.math_items) {
            result.items.push_back({ .key = item.key });
        }
        cb(std::move(result));
    }
}

void MermaidRenderer::DrainPendingRequests(bool cancelled)
{
    // 待機中の SVG・インライン数式のリクエストを完了させ、呼び出し元の in-flight フラグ固着を防ぐ。
    while (!pending_requests_.empty()) {
        auto& req = pending_requests_.front();
        FailCallbacksIfAny(req, cancelled);
        ReleaseInflight(req);
        pending_requests_.pop_front();
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
        FailCallbacksIfAny(w.current_request, true);
        w.current_request = {};
    }
    inflight_.clear();

    disk_drop_gen_.fetch_add(1, std::memory_order_relaxed);
    disk_results_.TakeAll();
}

void MermaidRenderer::DropQueued()
{
    // 読み込み待ちのディスクキャッシュも取り消す。描画直後の PNG はディスクへ保存したいので取り消さない。
    disk_drop_gen_.fetch_add(1, std::memory_order_relaxed);
    // インライン数式は幅にもテーマにも依らないので、移動しても描く価値が残る。
    for (auto& req : TakePending(&RenderRequest::is_display)) {
        ReleaseInflight(req);
    }
}

void MermaidRenderer::DropQueuedMath()
{
    for (auto& req : TakePending(&RenderRequest::is_math)) {
        FailMathCallbackIfAny(req, true);
    }
}

std::vector<MermaidRenderer::RenderRequest> MermaidRenderer::TakePending(bool (RenderRequest::*kind)() const noexcept)
{
    const auto taken = std::ranges::stable_partition(pending_requests_, [kind](const RenderRequest& r) { return !(r.*kind)(); });
    std::vector<RenderRequest> out(std::make_move_iterator(taken.begin()), std::make_move_iterator(taken.end()));
    pending_requests_.erase(taken.begin(), taken.end());
    return out;
}

void MermaidRenderer::RecoverWorker(int index)
{
    auto& w = workers_[index];
    // SVG は失敗完了で呼び出し元の in-flight 固着を防ぐ。PNG は捨てると再リクエストの
    // 契機が無く図が Loading のまま固着するためキューに戻す。クラッシュを誘発する
    // 入力での再起動ループは retried で 1 回に打ち切る。
    InvokeSvgCallbackIfAny(w.current_request, {}, false);
    bool failed = false;
    if (w.IsRendering() && !w.current_request.svg_only) {
        if (!w.current_request.retried) {
            w.current_request.retried = true;
            pending_requests_.push_back(std::move(w.current_request));
        }
        else {
            // リトライ済みの再クラッシュ。エラー確定にしないと Loading 固着かつ
            // 完了通知も出ないため、汎用エラーを設定して再描画させる。
            SetDiagramError(w.current_request, DiagramError::Kind::Render);
            FailMathCallbackIfAny(w.current_request, false);
            ReleaseInflight(w.current_request);
            failed = true;
        }
    }
    w.current_request = {};
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
        // 仮表示中の旧結果より、新しいコードのエラーを見せる。
        diagram_entry.ResetForRetry();
        diagram_entry.error = result.error;
        return;
    }
    diagram_entry.bitmap = result.bitmap;
    diagram_entry.width = result.width;
    diagram_entry.height = result.height;
    diagram_entry.png = result.png;
    diagram_entry.stale = false;
}

bool MermaidRenderer::RequestRender(
    const Node& node,
    DiagramEntry& diagram_entry,
    float max_width, bool dark_mode)
{
    if (node.type != NodeType::Diagram) {
        return false;
    }

    const uint64_t code_hash = mermaid_util::EnsureDiagramCodeHash(diagram_entry.code_hash, node);
    const uint64_t hash = mermaid_util::DiagramRequestHash(code_hash, max_width, dark_mode);
    const uint64_t width_free_hash = mermaid_util::DiagramWidthFreeHash(code_hash, dark_mode);
    if (const auto* cached = FindCached(hash, width_free_hash, max_width)) {
        ApplyResult(diagram_entry, *cached);
        return true;
    }
    if (!inflight_.insert(hash).second) {
        return false;
    }

    // ディスクから読めなかったときは描画し直すので、ディスクに当たってもコードを持たせる。
    RenderRequest req{
        .code = std::string{ node.GetText() },
        .max_width = max_width,
        .dark_mode = dark_mode,
        .code_hash = hash,
        .width_free_hash = width_free_hash,
    };
    if (const auto hit = file_cache_->Find(width_free_hash, hash, max_width, /*touch=*/true)) {
        if (auto png_path = file_cache_->PngPath(hit->key); !png_path.empty()) {
            req.width_free = hit->key == width_free_hash;
            req.css_width = hit->entry.css_width;
            req.css_height = hit->entry.css_height;
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
    DiskLoad job{ .req = std::move(req), .captured_stream = std::move(captured_stream), .captured = captured,
                  .drop_gen = disk_drop_gen_.load(std::memory_order_relaxed) };
    // ファイル読み込みと PNG デコードは worker で行う (UI では時間予算なしに積み上がる)。
    const bool posted = bg_scheduler_->Post([this, job = std::move(job), path = std::move(png_path), guard = latch_.Acquire()]() mutable {
        // インライン数式は取り消すと頼み直す契機が無いので、移動しても読む (DropQueued でも待ちに残す)。
        job.dropped = !job.captured && job.req.is_display() && job.drop_gen != disk_drop_gen_.load(std::memory_order_relaxed);
        if (!job.dropped) {
            LoadDiskJob(job, path);
        }
        if (disk_results_.Push(std::move(job))) {
            ::PostMessageW(hwnd_, app_msg::MERMAID_DISK_LOADED, 0, 0);
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
        job.png = std::make_shared<const ByteBuffer>(std::move(data));
    }
    TaskScheduler::EnsureWorkerCom();
    if (job.req.is_math()) {
        LoadMathJob(job);
        return;
    }
    // job.png はデコード完了まで生きているので、コピーせずに参照して読む。
    if (job.captured) {
        std::tie(job.bitmap, job.png) = DecodeCapture(wic_factory_.Get(), job.png, job.req.page_rect);
    }
    else {
        job.bitmap = DecodePngToWicBitmap(wic_factory_.Get(), *job.png);
    }
}

void MermaidRenderer::DecideMathPage(RenderRequest& req, const mermaid_util::RenderSize& page)
{
    if (auto want_page = std::exchange(req.math_want_page, {})) {
        const auto rect = PageRect(page);
        const size_t bytes = static_cast<size_t>(rect.Width) * static_cast<size_t>(rect.Height);
        req.math_page = want_page(bytes);
        req.math_reserved_bytes = req.math_page ? bytes : 0;
    }
}

void MermaidRenderer::LoadMathJob(DiskLoad& job)
{
    auto& req = job.req;
    // デコーダは CopyPixels のときに PNG を読むので、それまで手放さない。
    const PngBytes source = std::move(job.png);
    if (!job.captured) {
        auto layout = mermaid_util::ReadMathBoxesChunk({ source->data(), source->size() }, req.math_items.size());
        if (!layout) {
            return;
        }
        req.math_layout = std::move(*layout);
    }
    else if (req.math_cache_key != 0) {
        if (auto with_boxes = mermaid_util::AppendMathBoxesChunk({ source->data(), source->size() }, req.math_layout); !with_boxes.empty()) {
            job.png = std::make_shared<const ByteBuffer>(std::move(with_boxes));
        }
    }
    if (!req.math_page) {
        return;
    }
    // 透明な背景のキャプチャは BGRA の straight alpha で、黒で描いた数式の形はアルファにだけある。
    // WIC にアルファだけを取り出させる (BGRA で受けて抜き出すより、1536x618 のページで 2.65ms → 1.85ms、
    // 一時バッファ 3.8MB が要らない。結果は同じ)。
    const auto decoded = wic_util::DecodeFromMemory(wic_factory_.Get(), *source, GUID_ContainerFormatPng, GUID_WICPixelFormat8bppAlpha);
    if (!decoded) {
        return;
    }
    // 図と同じく、WebView2 が余分に撮った透明な列を落とす (DecideMathPage が見込んだ大きさにもなる)。
    // ディスクキャッシュの PNG は撮ったままなので、読むときも切り出す。
    const auto rect = ClipToCapture(PageRect(req.math_layout.size), decoded->pixel_width, decoded->pixel_height);
    if (!rect) {
        return;
    }
    const auto w = static_cast<UINT>(rect->Width);
    const auto h = static_cast<UINT>(rect->Height);
    job.alpha.resize(size_t{ w } * h);
    if (FAILED(decoded->converter->CopyPixels(&*rect, w, static_cast<UINT>(job.alpha.size()), job.alpha.data()))) {
        job.alpha.clear();
        return;
    }
    job.alpha_width = w;
    job.alpha_height = h;
}

void MermaidRenderer::ApplyMathLoad(DiskLoad& r)
{
    auto& req = r.req;
    // 箱の一覧が揃い、受け取るページならアルファも取り出せた。
    const bool loaded = !req.math_layout.boxes.empty() && (!req.math_page || !r.alpha.empty());
    // ディスクキャッシュから読めなかった (消えていた・壊れていた) ので描き直す。描き直したページで上書きする。
    if (!loaded && !r.captured) {
        file_cache_->OnReadFailed(req.math_cache_key, r.read_error);
        EnqueueWebRender(std::move(req));
        return;
    }
    if (!req.math_callback) {
        return;
    }
    Microsoft::WRL::ComPtr<ID2D1Bitmap> page;
    if (!r.alpha.empty() && render_target_) {
        // FillOpacityMask の切り出し位置はビットマップの DIP で指すので、96 DPI にしてピクセルと揃える。
        const auto props = D2D1::BitmapProperties(D2D1::PixelFormat(DXGI_FORMAT_A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), DEFAULT_DPI, DEFAULT_DPI);
        render_target_->CreateBitmap(D2D1::SizeU(r.alpha_width, r.alpha_height), r.alpha.data(), r.alpha_width, props, &page);
    }
    if (!loaded || (req.math_page && !page)) {
        FailMathCallbackIfAny(req, false);
        return;
    }
    // エラーの数式も箱の一覧に残るので、次からは描き直さずに原文で表示できる。
    if (r.captured && r.png) {
        file_cache_->StoreAsync(req.math_cache_key, req.math_layout.size.width, req.math_layout.size.height, std::move(r.png));
    }
    const float dpr = req.math_layout.size.dpr;
    const float page_w = static_cast<float>(r.alpha_width);
    const float page_h = static_cast<float>(r.alpha_height);
    InlineMathBatchResult result{ .page = std::move(page), .reserved_bytes = req.math_reserved_bytes };
    result.items.reserve(req.math_items.size());
    for (size_t i = 0; i < req.math_items.size(); ++i) {
        const auto& box = req.math_layout.boxes[i];
        InlineMathBatchResult::Item item{ .key = req.math_items[i].key };
        if (box.width > 0.0f) {
            item.metrics = { box.width, box.height, box.baseline };
            if (result.page) {
                const float left = std::max(0.0f, std::floor((box.x - INLINE_MATH_INK_MARGIN) * dpr));
                const float top = std::max(0.0f, std::floor((box.y - INLINE_MATH_INK_MARGIN) * dpr));
                const float right = std::min(page_w, std::ceil((box.x + box.width + INLINE_MATH_INK_MARGIN) * dpr));
                const float bottom = std::min(page_h, std::ceil((box.y + box.height + INLINE_MATH_INK_MARGIN) * dpr));
                item.src = { left, top, right, bottom };
                item.dest = { left / dpr - box.x, top / dpr - box.y, right / dpr - box.x, bottom / dpr - box.y };
            }
        }
        result.items.push_back(item);
    }
    auto cb = std::exchange(req.math_callback, {});
    cb(std::move(result));
}

bool MermaidRenderer::ApplyDiskLoad(DiskLoad& r)
{
    // 結果は呼び出し側に直接渡し、図のキャッシュには入れない。
    if (r.req.is_math()) {
        ApplyMathLoad(r);
        return false;
    }
    // 表示に要るなら要求し直される。
    if (r.dropped) {
        ReleaseInflight(r.req);
        return false;
    }
    Microsoft::WRL::ComPtr<ID2D1Bitmap> bitmap;
    if (r.bitmap && render_target_ && SUCCEEDED(render_target_->CreateBitmapFromWicBitmap(r.bitmap.Get(), &bitmap)) && bitmap) {
        if (r.captured) {
            r.req.width_free = mermaid_util::IsWidthFree(r.req.css_width, r.req.max_width);
            file_cache_->StoreAsync(r.req.CacheKey(), r.req.css_width, r.req.css_height, r.png);
        }
        InsertCache(r.req.CacheKey(), CachedResult{ std::move(bitmap), r.req.css_width, r.req.css_height, std::move(r.png) });
        ReleaseInflight(r.req);
        return true;
    }
    // 描画直後の PNG を描き直しても同じ結果になるので、エラーを確定させて再要求させない。
    if (r.captured) {
        SetDiagramError(r.req, DiagramError::Kind::Render);
        ReleaseInflight(r.req);
        return true;
    }
    if (!r.png) {
        file_cache_->OnReadFailed(r.req.CacheKey(), r.read_error);
    }
    EnqueueWebRender(std::move(r.req));
    return false;
}

void MermaidRenderer::ProcessDiskLoads(std::chrono::steady_clock::time_point deadline)
{
    bool resolved = false;
    disk_results_.Drain(deadline, [&](DiskLoad& r) { resolved |= ApplyDiskLoad(r); });
    // まとめて 1 回だけ要求し直させる。
    if (resolved) {
        NotifyUpdate();
    }
}

void MermaidRenderer::EnqueueWebRender(RenderRequest req)
{
    // 図は準備完了の通知で要求し直させる。SVG とインライン数式の要求は頼み直す仕組みが無いので、準備前でも捨てない。
    if (!ready_ && req.is_display()) {
        ReleaseInflight(req);
        EnsureInitialized();
        return;
    }
    pending_requests_.push_back(std::move(req));
    if (!ready_) {
        EnsureInitialized();
        return;
    }
    ProcessQueue();
}

void MermaidRenderer::RequestSvg(std::string_view code, float max_width, bool dark_mode, SvgCallback callback)
{
    EnqueueWebRender(RenderRequest{
        .code = std::string{ code },
        .max_width = max_width,
        .dark_mode = dark_mode,
        .svg_only = true,
        .svg_callback = std::move(callback),
    });
}

void MermaidRenderer::RequestMathBatch(std::vector<InlineMathItem> items, uint64_t cache_key, MathBatchCallback callback, MathPageGate want_page)
{
    RenderRequest req{ .math_items = std::move(items), .math_callback = std::move(callback), .math_cache_key = cache_key,
                       .math_want_page = std::move(want_page) };
    if (!req.is_math()) {
        FailMathCallbackIfAny(req, false);
        return;
    }
    // 当たれば WebView2 を起動しない。幅に依らないので、幅なしのキーとして引く。
    if (cache_key != 0) {
        if (const auto hit = file_cache_->Find(cache_key, cache_key, std::numeric_limits<float>::infinity(), /*touch=*/true)) {
            if (auto png_path = file_cache_->PngPath(hit->key); !png_path.empty()) {
                DecideMathPage(req, { hit->entry.css_width, hit->entry.css_height, file_cache_->Dpr() });
                StartDiskLoad(std::move(req), std::move(png_path));
                return;
            }
        }
    }
    EnqueueWebRender(std::move(req));
}

void MermaidRenderer::ProcessQueue()
{
    if (!ready_) {
        return;
    }

    // 同一コードの図が複数あるとき、最初の1つのレンダリング完了後に
    // 残りをキャッシュから即座に解決できる。
    while (!pending_requests_.empty()) {
        auto& front = pending_requests_.front();
        // 結果を出した要求が通知済みなので、ここでは取り除くだけでよい。
        if (FindCached(front)) {
            ReleaseInflight(front);
            pending_requests_.pop_front();
            continue;
        }

        Worker* idle = FindIdleWorker();
        if (!idle) {
            break;
        }

        idle->current_request = std::move(front);
        pending_requests_.pop_front();
        idle->current_request.request_id = ++request_counter_;
        RenderInWorker(*idle);
    }
    MaybeGrowWorkers();
    // 待ちが残っている間はアイドル扱いしない。描画中なら OnIdleTimer が見送り、完了時にここで張り直す。
    if (pending_requests_.empty()) {
        ScheduleIdleShutdown();
    }
}

void MermaidRenderer::FreeWorker(Worker& worker)
{
    worker.current_request = {};
    ProcessQueue();
}

void MermaidRenderer::FailWorkerRequest(Worker& worker, DiagramError::Kind kind, std::wstring_view msg)
{
    SetDiagramError(worker.current_request, kind, msg);
    FinishWorkerRequest(worker);
}

void MermaidRenderer::FinishWorkerRequest(Worker& worker)
{
    // SVG は成功ならコールバックを呼び済みで、描けたインライン数式は要求を StartDiskLoad へ移してから FreeWorker する。
    FailCallbacksIfAny(worker.current_request, false);
    const bool resolved = FindCached(worker.current_request);
    ReleaseInflight(worker.current_request);
    FreeWorker(worker);
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
    if (req.is_math()) {
        RenderMathInWorker(worker);
        return;
    }

    // CSS ビューポートが max_width (DIP) と等しくなるよう境界を物理ピクセルで設定する
    // (WebView2 は内部で devicePixelRatio で除算する)。SVG 出力 (折返し等) も CSS 幅に
    // 依存するため、PNG/SVG 両経路で同じ bounds を使う。
    const int vp_phys = std::max(1, DipToPixelCeil(req.max_width, worker.dpr));
    ResizeWorkerView(worker, vp_phys, HOST_SIZE);

    using mermaid_util::PageRequest;
    const PageRequest kind = req.svg_only ? PageRequest::Svg : PageRequest::Mermaid;
    PostToPage(worker, mermaid_util::PageRenderMessage(kind, req.request_id, req.dark_mode, req.code));
}

void MermaidRenderer::RenderMathInWorker(Worker& worker)
{
    ResizeWorkerView(worker, DipToPixelCeil(MATH_PAGE_WIDTH, worker.dpr), HOST_SIZE);
    auto msg = mermaid_util::PageMathBatchMessage(worker.current_request.request_id, MATH_PAGE_WIDTH);
    for (const auto& item : worker.current_request.math_items) {
        mermaid_util::AppendPageMathItem(msg, item.tex, item.font_size);
    }
    PostToPage(worker, msg);
}

void MermaidRenderer::PostToPage(Worker& worker, const std::wstring& msg)
{
    // 要求 ID を返事に含めさせ、コールバックと要求を照合する。描けなければページが render-error で文言を返す。
    LogHrFailure("PostWebMessageAsString", worker.webview->PostWebMessageAsString(msg.c_str()));
}

void MermaidRenderer::OnMathResult(int worker_idx, std::wstring_view payload)
{
    auto& w = workers_[worker_idx];
    auto layout = mermaid_util::ParseMathBatch(payload, w.current_request.math_items.size());
    if (!layout || !FitsCapture(layout->size)) {
        FinishWorkerRequest(w);
        return;
    }
    w.current_request.math_layout = std::move(*layout);
    ScheduleCapture(w, w.current_request.math_layout.size);
}

void MermaidRenderer::ScheduleCapture(Worker& w, const mermaid_util::RenderSize& size)
{
    const auto view = PageRect(size);
    ResizeWorkerView(w, view.Width, view.Height);
    PostToPage(w, mermaid_util::PageCaptureMessage(w.current_request.request_id));
}

void MermaidRenderer::RenderRequest::SetCaptureBox(const WICRect& rect, float dpr) noexcept
{
    page_rect = rect;
    css_width = PixelToDip(static_cast<float>(rect.Width), dpr);
    css_height = PixelToDip(static_cast<float>(rect.Height), dpr);
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
        file_cache_->SetDpr(dpr);
    }
    worker.ready = true;
    worker.init_retries = 0;
    // 初期化 (アイドル解放後の再初期化を含む) ごとに、最初のワーカーの準備完了で on_update_ を呼ぶ。
    // 残りは準備でき次第プールに参加する。
    if (!std::exchange(ready_, true)) {
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
            OnRenderResult(index, mermaid_util::ParseRenderSize(payload));
        }
        return;
    case WebMessageKind::CaptureReady:
        if (req_id_match) {
            DoCapturePreview(index);
        }
        return;
    case WebMessageKind::SvgResult:
        if (req_id_match && w.current_request.svg_only) {
            InvokeSvgCallbackIfAny(w.current_request, std::wstring{ payload }, false);
            FinishWorkerRequest(w);
        }
        return;
    case WebMessageKind::RenderError:
        if (req_id_match) {
            FailWorkerRequest(w, DiagramError::Kind::Render, payload);
        }
        return;
    case WebMessageKind::MathResult:
        if (req_id_match && w.current_request.is_math()) {
            OnMathResult(index, payload);
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

void MermaidRenderer::OnRenderResult(int worker_idx, const mermaid_util::RenderSize& size)
{
    auto& w = workers_[worker_idx];

    // リクエスト ID はメッセージハンドラで照合済み。
    if (size.width <= 0 || size.height <= 0) {
        // 大きさの無い図はキャプチャできない。放置すると「読み込み中」のまま固着する (issue #271)。
        FailWorkerRequest(w, DiagramError::Kind::Render);
        return;
    }
    if (!FitsCapture(size)) {
        FailWorkerRequest(w, DiagramError::Kind::TooLarge);
        return;
    }
    w.current_request.SetCaptureBox(PageRect(size), size.dpr);
    ScheduleCapture(w, size);
}

bool MermaidRenderer::FitsCapture(const mermaid_util::RenderSize& size) const noexcept
{
    return mermaid_util::FitsCaptureLimit(size.width, size.height, size.dpr, MaxBitmapDim(render_target_));
}

void MermaidRenderer::DoCapturePreview(int worker_idx)
{
    auto& w = workers_[worker_idx];
    if (!w.webview) {
        FinishWorkerRequest(w);
        return;
    }

    auto png_stream = CreateMemoryStream();
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
    // 読み出しが終わるまで要求は inflight_ に残す。
    auto req = std::move(w.current_request);
    FreeWorker(w);
    if (req.is_math()) {
        DecideMathPage(req, req.math_layout.size);
    }
    StartDiskLoad(std::move(req), {}, std::move(png_stream));
}
