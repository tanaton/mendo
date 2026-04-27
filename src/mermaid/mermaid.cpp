#include "mermaid.h"
#include "mermaid_file_cache.h"
#include "mermaid_util.h"
#include "stream_util.h"
#include "utility.h"
#include "wic_util.h"
#include "resource.h"
#include <wrl/event.h>
#include <cassert>
#include <filesystem>
#include <functional>
#include <memory_resource>

#pragma comment(lib, "windowscodecs.lib")

static const wchar_t* MERMAID_HOST_CLASS = L"mendo_MermaidHost";

static constexpr std::wstring_view APP_LOCAL_ORIGIN_PREFIX = L"https://app.local/";
static constexpr wchar_t APP_LOCAL_INDEX_URL[] = L"https://app.local/index.html";

int MermaidRenderer::ComputeWorkerCount() noexcept
{
    SYSTEM_INFO si{};
    GetSystemInfo(&si);
    return mermaid_util::ComputeWorkerCount(si.dwNumberOfProcessors);
}

MermaidRenderer::~MermaidRenderer()
{
    Shutdown();
}

void MermaidRenderer::Shutdown()
{
    CancelPending();

    for (int i = 0; i < worker_count_; i++) {
        if (workers_[i].controller) {
            workers_[i].controller->Close();
            workers_[i].controller.Reset();
        }
        if (workers_[i].webview) {
            workers_[i].webview.Reset();
        }
        if (workers_[i].hwnd) {
            DestroyWindow(workers_[i].hwnd);
            workers_[i].hwnd = nullptr;
        }
    }
    worker_count_ = 0;
    webview_env_.Reset();
    lifecycle_.Reset();
}

void MermaidRenderer::Init(HWND hwnd, ID2D1RenderTarget* render_target, IWICImagingFactory* wic,
    const std::filesystem::path& user_data_folder, std::move_only_function<void()> on_ready)
{
    hwnd_ = hwnd;
    render_target_ = render_target;
    if (!user_data_folder.empty()) {
        user_data_folder_.assign(user_data_folder.native());
    }
    on_all_ready_ = std::move(on_ready);

    // PNGデコード用のWICファクトリ（D2DRenderBackendから共有）
    if (wic) {
        wic_factory_ = wic;
    }
    else {
        CoCreateInstance(
            CLSID_WICImagingFactory,
            nullptr,
            CLSCTX_INPROC_SERVER,
            IID_PPV_ARGS(&wic_factory_)
        );
    }
}

void MermaidRenderer::EnsureInitialized()
{
    if (!lifecycle_.TryMarkInitialized()) {
        return;
    }

    worker_count_ = ComputeWorkerCount();

    // オフスクリーンでWebView2をホストする非表示ポップアップウィンドウを登録・作成する。
    // WebView2はCapturePreviewでコンテンツをレンダリングするためにIsVisible=TRUEが必要なため、
    // 非表示にする代わりに画面外に配置したポップアップを使用する。
    static bool class_registered = false;
    if (!class_registered) {
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = DefWindowProcW;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = MERMAID_HOST_CLASS;
        RegisterClassExW(&wc);
        class_registered = true;
    }

    for (int i = 0; i < worker_count_; i++) {
        workers_[i].hwnd = CreateWindowExW(
            WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
            MERMAID_HOST_CLASS,
            L"",
            WS_POPUP,
            -32000, -32000,     // 画面外の遠い位置
            4096, 4096,         // どのダイアグラムにも十分な大きさ
            nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);

        if (!workers_[i].hwnd) {
            worker_count_ = i;
            break;
        }

        // ポップアップを表示する（WebView2が「可視」と認識するために必要）
        ShowWindow(workers_[i].hwnd, SW_SHOWNOACTIVATE);
    }

    if (worker_count_ == 0) {
        return;
    }

    // WebView2環境の生成失敗はタイマーリトライで対処されるため、
    // ワーカーウィンドウ作成完了時点で（initialized は既にマーク済みで）次段へ進む。
    CreateWebView2Environment();
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
        if (FAILED(result) || !env) {
            // 前回プロセスがユーザーデータフォルダをまだ解放していない場合など
            // に失敗する。タイマーで遅延リトライする。
            if (env_retry_count_ < MAX_ENV_RETRIES && hwnd_) {
                ++env_retry_count_;
                SetTimer(hwnd_, TIMER_INIT_RETRY, 500, nullptr);
            }
            return S_OK;
        }
        env_retry_count_ = 0;
        webview_env_ = env;

        for (int i = 0; i < worker_count_; i++) {
            SetupWorker(i);
        }
        return S_OK;
    }).Get());
}

void MermaidRenderer::OnInitRetryTimer()
{
    KillTimer(hwnd_, TIMER_INIT_RETRY);
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
        if (FAILED(result) || !controller) {
            return S_OK;
        }

        auto& w = workers_[index];
        w.controller = controller;
        if (FAILED(controller->get_CoreWebView2(&w.webview)) || !w.webview) {
            w.controller.Reset();
            return S_OK;
        }

        const RECT bounds = { 0, 0, 4096, 4096 };
        controller->put_Bounds(bounds);

        Microsoft::WRL::ComPtr<ICoreWebView2Settings> settings;
        w.webview->get_Settings(&settings);
        if (settings) {
            settings->put_AreDevToolsEnabled(FALSE);
            settings->put_IsStatusBarEnabled(FALSE);
            settings->put_AreDefaultContextMenusEnabled(FALSE);
            settings->put_AreDefaultScriptDialogsEnabled(FALSE);
        }

        w.webview->add_WebMessageReceived(
            Microsoft::WRL::Callback<ICoreWebView2WebMessageReceivedEventHandler>(
                [this, index](ICoreWebView2*,
                    ICoreWebView2WebMessageReceivedEventArgs* args) -> HRESULT {
            LPWSTR msg = nullptr;
            if (SUCCEEDED(args->TryGetWebMessageAsString(&msg)) && msg) {
                auto& w = workers_[index];
                if (wcsncmp(msg, L"mermaid-ready:", 14) == 0) {
                    // "mermaid-ready:<dpr>"からDPRを解析
                    const float dpr = std::wcstof(msg + 14, nullptr);
                    if (dpr > 0) {
                        w.dpr = dpr;
                    }
                    w.ready = true;
                    w.init_retries = 0;
                    // 最初のワーカーが準備完了した時点でon_readyを呼び出す。
                    // 残りのワーカーは準備でき次第プールに参加する。
                    if (!lifecycle_.IsReady()) {
                        lifecycle_.MarkReady();
                        if (on_all_ready_) {
                            auto cb = std::move(on_all_ready_);
                            cb();
                        }
                    }
                    ProcessQueue();
                }
                else if (wcsncmp(msg, L"render-result:", 14) == 0) {
                    const auto p = mermaid_util::ParseRequestPrefix(msg + 14);
                    if (p.valid && p.has_payload && p.id == w.current_request.request_id) {
                        OnRenderResult(index, p.payload);
                    }
                }
                else if (wcsncmp(msg, L"capture-ready:", 14) == 0) {
                    const auto p = mermaid_util::ParseRequestPrefix(msg + 14);
                    if (p.valid && p.id == w.current_request.request_id) {
                        DoCapturePreview(index);
                    }
                }
                else if (wcsncmp(msg, L"svg-result:", 11) == 0) {
                    const auto p = mermaid_util::ParseRequestPrefix(msg + 11);
                    if (p.valid && p.id == w.current_request.request_id && w.current_request.svg_only) {
                        std::pmr::wstring svg{ p.has_payload ? p.payload : std::wstring_view{} };
                        InvokeSvgCallbackIfAny(w.current_request, std::move(svg), false);
                        FinishWorkerRequest(w);
                    }
                }
                else if (wcsncmp(msg, L"render-error:", 13) == 0) {
                    const auto p = mermaid_util::ParseRequestPrefix(msg + 13);
                    if (p.valid && p.id == w.current_request.request_id) {
                        InvokeSvgCallbackIfAny(w.current_request, {}, false);
                        FinishWorkerRequest(w);
                    }
                }
                else if (wcscmp(msg, L"mermaid-failed") == 0) {
                    // mermaid.jsの読み込みに失敗した場合、ページを再読み込みして再試行する
                    if (w.init_retries < MAX_WORKER_RETRIES && w.webview) {
                        ++w.init_retries;
                        w.webview->Navigate(APP_LOCAL_INDEX_URL);
                    }
                }
                CoTaskMemFree(msg);
            }
            return S_OK;
        }).Get(), nullptr);

        // ナビゲーションを制限: app.local以外へのナビゲーションをブロック
        w.webview->add_NavigationStarting(
            Microsoft::WRL::Callback<ICoreWebView2NavigationStartingEventHandler>(
                [](ICoreWebView2*, ICoreWebView2NavigationStartingEventArgs* args) static->HRESULT {
            LPWSTR uri = nullptr;
            if (SUCCEEDED(args->get_Uri(&uri)) && uri) {
                const std::wstring_view u(uri);
                if (!u.starts_with(APP_LOCAL_ORIGIN_PREFIX) && u != L"about:blank") {
                    args->put_Cancel(TRUE);
                }
                CoTaskMemFree(uri);
            }
            return S_OK;
        }).Get(), nullptr);

        w.webview->add_NewWindowRequested(
            Microsoft::WRL::Callback<ICoreWebView2NewWindowRequestedEventHandler>(
                [](ICoreWebView2*, ICoreWebView2NewWindowRequestedEventArgs* args) static->HRESULT {
            args->put_Handled(TRUE);
            return S_OK;
        }).Get(), nullptr);

        // 仮想ホストへのリクエストをインターセプトし、
        // 埋め込みWin32リソースからHTML / mermaid.jsを配信する。
        // 全URLをフィルタし、app.local以外へのリクエストもブロックする。
        w.webview->AddWebResourceRequestedFilter(
            L"*",
            COREWEBVIEW2_WEB_RESOURCE_CONTEXT_ALL);

        w.webview->add_WebResourceRequested(
            Microsoft::WRL::Callback<ICoreWebView2WebResourceRequestedEventHandler>(
                [this](ICoreWebView2*,
                    ICoreWebView2WebResourceRequestedEventArgs* args) -> HRESULT {
            Microsoft::WRL::ComPtr<ICoreWebView2WebResourceRequest> request;
            args->get_Request(&request);
            LPWSTR uri = nullptr;
            request->get_Uri(&uri);
            const std::pmr::wstring url(uri ? uri : L"");
            CoTaskMemFree(uri);

            // https://app.local/ 以外へのリクエストをブロックする。
            // NavigationStartingと判定ロジックを揃え、app.local.evil.comのような
            // 部分一致によるサブドメイン経由の経路を塞ぐ。
            if (!url.starts_with(APP_LOCAL_ORIGIN_PREFIX)) {
                Microsoft::WRL::ComPtr<ICoreWebView2WebResourceResponse> response;
                webview_env_->CreateWebResourceResponse(nullptr, 403, L"Blocked", L"", &response);
                args->put_Response(response.Get());
                return S_OK;
            }

            std::span<const std::byte> payload;
            const wchar_t* headers = nullptr;

            if (url.find(L"/mermaid.min.js.gz") != std::pmr::wstring::npos) {
                // gzip圧縮されたmermaid.jsを配信する。JSがDecompressionStreamで展開する
                payload = LoadRcData(IDR_MERMAID_JS_GZ);
                headers = L"Content-Type: application/gzip";
            }
            else {
                // その他のパスにはHTMLテンプレート（res/mermaid.html）を配信する
                payload = LoadRcData(IDR_MERMAID_HTML);
                headers = L"Content-Type: text/html; charset=utf-8";
            }

            // リソースが見つからない場合は空ボディを200で返さず500を返して失敗を明示する
            if (payload.empty()) {
                Microsoft::WRL::ComPtr<ICoreWebView2WebResourceResponse> response;
                webview_env_->CreateWebResourceResponse(nullptr, 500, L"Resource missing", L"", &response);
                args->put_Response(response.Get());
                return S_OK;
            }

            const auto stream = stream_util::CreateMemoryStream(payload.data(), payload.size());
            if (stream) {
                Microsoft::WRL::ComPtr<ICoreWebView2WebResourceResponse> response;
                webview_env_->CreateWebResourceResponse(stream.Get(), 200, L"OK", headers, &response);
                args->put_Response(response.Get());
            }
            return S_OK;
        }).Get(),
            nullptr);

        // 仮想ホストにナビゲートする（HTML + JSは上記ハンドラにより
        // メモリから配信される）。
        w.webview->Navigate(APP_LOCAL_INDEX_URL);

        return S_OK;
    }).Get());
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

void MermaidRenderer::InvokeSvgCallbackIfAny(RenderRequest& req, std::pmr::wstring svg, bool cancelled)
{
    if (!req.svg_only) {
        return;
    }
    if (auto cb = std::move(req.svg_callback)) {
        cb(std::move(svg), cancelled);
    }
}

void MermaidRenderer::CancelPending()
{
    // SVG 専用リクエストのコールバックは cancelled=true で呼び、呼び出し元の状態をリセットさせる。
    // PNG レンダリング (on_complete) は無引数のため呼び出し元側でフラグを持っていない前提。
    while (!pending_requests_.empty()) {
        InvokeSvgCallbackIfAny(pending_requests_.front(), {}, true);
        pending_requests_.pop();
    }

    // current_request の request_id が 0 に戻るため、処理中の非同期コールバックは
    // ID 不一致で自動的に無視される。
    for (int i = 0; i < worker_count_; i++) {
        auto& w = workers_[i];
        InvokeSvgCallbackIfAny(w.current_request, {}, true);
        w.rendering = false;
        w.current_request = {};
    }
}

uint64_t MermaidRenderer::HashCode(std::string_view code_utf8, float max_width, bool dark_mode) const noexcept
{
    return mermaid_util::HashCode(code_utf8, max_width, dark_mode);
}

void MermaidRenderer::RequestRender(Node& node, NodeLayoutEntry& layout_entry,
    DiagramEntry& diagram_entry,
    float max_width, bool dark_mode,
    Callback on_complete)
{
    if (!IsDiagramLanguage(node.code_language)) {
        return;
    }

    const auto hash = mermaid_util::NodeDiagramHash(node, max_width, dark_mode);

    if (const auto* cached = cache_.Find(hash)) {
        diagram_entry.bitmap = cached->bitmap;
        diagram_entry.width = cached->width;
        diagram_entry.height = cached->height;
        layout_entry.height = cached->height;
        layout_entry.layout_dirty = false;
        if (on_complete) {
            on_complete();
        }
        return;
    }

    if (file_cache_) {
        MermaidFileCache::CacheEntry fentry;
        MermaidFileCache::PngBlob png;
        if (file_cache_->Lookup(hash, fentry, png)) {
            auto stream = stream_util::CreateMemoryStream(png.data.get(), png.size);
            if (stream) {
                Microsoft::WRL::ComPtr<ID2D1Bitmap> bitmap;
                float bw = 0, bh = 0;
                if (SUCCEEDED(CreateBitmapFromPngStream(stream.Get(), &bitmap, &bw, &bh)) && bitmap) {
                    diagram_entry.bitmap = bitmap;
                    diagram_entry.width = fentry.css_width;
                    diagram_entry.height = fentry.css_height;
                    layout_entry.height = fentry.css_height;
                    layout_entry.layout_dirty = false;

                    cache_.Insert(hash, CachedBitmap{ bitmap, fentry.css_width, fentry.css_height });

                    if (on_complete) {
                        on_complete();
                    }
                    return;
                }
            }
        }
    }

    if (!lifecycle_.IsReady()) {
        EnsureInitialized();
        return;
    }

    RenderRequest req;
    req.node = &node;
    req.layout_entry = &layout_entry;
    req.diagram_entry = &diagram_entry;
    req.max_width = max_width;
    req.dark_mode = dark_mode;
    req.on_complete = std::move(on_complete);
    req.code_hash = hash;
    pending_requests_.push(std::move(req));

    ProcessQueue();
}

void MermaidRenderer::RequestSvg(std::wstring_view code, float max_width, bool dark_mode, SvgCallback callback)
{
    // 呼び出し元 (App::CopyDiagramAsSvg) は IsSvgExportable と非空コード、有効なコールバックを保証する。
    assert(callback && "RequestSvg requires a callable SvgCallback");
    assert(!code.empty() && "RequestSvg requires non-empty code");

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
    if (!lifecycle_.IsReady() || pending_requests_.empty()) {
        return;
    }

    // 同一コードの図が複数あるとき、最初の1つのレンダリング完了後に
    // 残りをキャッシュから即座に解決できる。SVG 専用リクエストは PNG ビットマップ
    // キャッシュ対象外なので常にワーカー経由でレンダリングする。
    while (!pending_requests_.empty()) {
        auto& front = pending_requests_.front();
        const CachedBitmap* png_hit = front.svg_only ? nullptr : cache_.Find(front.code_hash);
        if (png_hit) {
            front.diagram_entry->bitmap = png_hit->bitmap;
            front.diagram_entry->width = png_hit->width;
            front.diagram_entry->height = png_hit->height;
            front.layout_entry->height = png_hit->height;
            front.layout_entry->layout_dirty = false;
            auto cb = std::move(front.on_complete);
            pending_requests_.pop();
            if (cb) {
                cb();
            }
            continue;
        }

        Worker* idle = nullptr;
        for (int i = 0; i < worker_count_; i++) {
            if (workers_[i].ready && !workers_[i].rendering) {
                idle = &workers_[i];
                break;
            }
        }
        if (!idle) {
            break;  // 全ワーカーがビジー、完了を待つ
        }

        idle->current_request = std::move(front);
        pending_requests_.pop();
        idle->current_request.request_id = ++request_counter_;
        idle->rendering = true;
        RenderInWorker(*idle);
    }
}

void MermaidRenderer::FinishWorkerRequest(Worker& worker)
{
    worker.rendering = false;
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

    // CSSビューポートがmax_width（DIP）と等しくなるようにWebView2の境界を設定する。
    // 境界はポップアップウィンドウの物理ピクセル単位で、WebView2は
    // 内部でdevicePixelRatioで除算してCSSビューポートサイズを求める。
    // SVG 出力（折返し等）も CSS 幅に依存するため、PNG/SVG 両経路で同じ bounds を使う。
    int vp_phys = static_cast<int>(std::ceil(worker.current_request.max_width * worker.dpr));
    if (vp_phys < 1) {
        vp_phys = 1;
    }
    const int h_phys = static_cast<int>(4096 * worker.dpr);
    const RECT bounds = { 0, 0, vp_phys, h_phys };
    worker.controller->put_Bounds(bounds);
    SetWindowPos(worker.hwnd, nullptr, -32000, -32000, vp_phys, h_phys, SWP_NOZORDER | SWP_NOACTIVATE);

    // SVG 専用リクエスト: PNG キャプチャ用の再リサイズはスキップし、SVG 文字列のみ取得する。
    if (worker.current_request.svg_only) {
        const auto js = PmrFormat(
            L"renderMermaidSvg('{}', {})"
            L".then(function(s){{window.chrome.webview.postMessage('svg-result:{}:'+(s||''));}})"
            L".catch(function(e){{window.chrome.webview.postMessage('render-error:{}:'+String(e));}})",
            mermaid_util::JsEscape(worker.current_request.code_storage),
            worker.current_request.dark_mode ? L"true" : L"false",
            worker.current_request.request_id, worker.current_request.request_id);
        worker.webview->ExecuteScript(js.c_str(), nullptr);
        return;
    }

    // Mermaidをレンダリングする（maxWidth=0はCSS制約なし、ビューポートが制約する）
    // LatexMath ノードは flowchart ラッパに変換してから JS に渡す。
    // リクエストIDをpostMessageに含め、C++側でコールバックとリクエストを照合する
    const Node& src_node = *worker.current_request.node;
    std::pmr::wstring code_storage;
    std::wstring_view code_view;
    if (src_node.code_language == SyntaxLanguage::LatexMath) {
        code_storage = mermaid_util::BuildLatexFlowchartCode(src_node.GetText());
        code_view = code_storage;
    }
    else {
        code_view = src_node.GetText();
    }

    const auto js = PmrFormat(
        L"renderMermaid('{}', {}, 0)"
        L".then(function(r){{window.chrome.webview.postMessage('render-result:{}:'+r);}})"
        L".catch(function(e){{window.chrome.webview.postMessage('render-error:{}:'+String(e));}})",
        mermaid_util::JsEscape(code_view),
        worker.current_request.dark_mode ? L"true" : L"false",
        worker.current_request.request_id, worker.current_request.request_id);

    worker.webview->ExecuteScript(js.c_str(), nullptr);
}

void MermaidRenderer::OnRenderResult(int worker_idx, std::wstring_view json)
{
    auto& w = workers_[worker_idx];

    // jsonはrenderMermaidからの生の文字列。例: {"ok":true,"width":400,"height":300}
    // リクエストIDの照合はメッセージハンドラで実施済み
    const float dw = mermaid_util::ParseJsonNumber(json, L"\"width\"");
    const float dh = mermaid_util::ParseJsonNumber(json, L"\"height\"");
    float dpr = mermaid_util::ParseJsonNumber(json, L"\"dpr\"");
    if (dpr <= 0) {
        dpr = 1.0f;
    }
    const bool ok = mermaid_util::ParseJsonTrueFlag(json, L"\"ok\"");

    if (!ok || dw <= 0 || dh <= 0) {
        FinishWorkerRequest(w);
        return;
    }

    // 描画サイズ（DIP）として後で使用するためにCSSピクセル寸法を保存する
    w.current_request.css_width = dw;
    w.current_request.css_height = dh;
    w.current_request.dpr = dpr;

    // キャプチャ用にWebViewをダイアグラムの正確なサイズにリサイズする。
    // CSSピクセルにdevicePixelRatioを掛けて物理ピクセルを求める。
    const int cw = static_cast<int>(std::ceil(dw * dpr));
    const int ch = static_cast<int>(std::ceil(dh * dpr));
    const RECT capBounds = { 0, 0, static_cast<LONG>(cw), static_cast<LONG>(ch) };
    w.controller->put_Bounds(capBounds);

    SetWindowPos(w.hwnd, nullptr, -32000, -32000, cw, ch, SWP_NOZORDER | SWP_NOACTIVATE);

    // rAFを使ってWebViewが新しいサイズで再レンダリングするのを待ち、
    // postMessageでシグナルを送る（Promise-awaitの問題を回避する）。
    // リクエストIDを含めて、C++側でコールバックとリクエストを照合する。
    const auto cap_js = PmrFormat(
        L"requestAnimationFrame(function(){{requestAnimationFrame(function(){{"
        L"window.chrome.webview.postMessage('capture-ready:{}');}});}});",
        w.current_request.request_id);
    w.webview->ExecuteScript(cap_js.c_str(), nullptr);
}

void MermaidRenderer::DoCapturePreview(int worker_idx)
{
    auto& w = workers_[worker_idx];
    if (!w.webview) {
        FinishWorkerRequest(w);
        return;
    }

    // CapturePreviewコールバックでもリクエストIDを照合し、
    // CancelPending後に到着した古いキャプチャ結果を無視する
    const unsigned int req_id = w.current_request.request_id;
    auto pngStream = stream_util::CreateMemoryStream(nullptr, 0);
    if (!pngStream) {
        FinishWorkerRequest(w);
        return;
    }

    const HRESULT hr = w.webview->CapturePreview(
        COREWEBVIEW2_CAPTURE_PREVIEW_IMAGE_FORMAT_PNG,
        pngStream.Get(),
        Microsoft::WRL::Callback<ICoreWebView2CapturePreviewCompletedHandler>(
            [this, worker_idx, pngStream, req_id](HRESULT hr3) -> HRESULT {
        auto& w = workers_[worker_idx];
        if (w.current_request.request_id != req_id) {
            return S_OK;
        }
        if (SUCCEEDED(hr3) && pngStream) {
            OnCaptureComplete(worker_idx, w.current_request.code_hash, pngStream.Get());
        }
        else {
            FinishWorkerRequest(w);
        }
        return S_OK;
    }).Get());

    if (FAILED(hr)) {
        FinishWorkerRequest(w);
    }
}

void MermaidRenderer::OnCaptureComplete(int worker_idx, uint64_t code_hash, IStream* png_stream)
{
    auto& w = workers_[worker_idx];
    Microsoft::WRL::ComPtr<ID2D1Bitmap> bitmap;
    float bw = 0, bh = 0;

    if (SUCCEEDED(CreateBitmapFromPngStream(png_stream, &bitmap, &bw, &bh)) && bitmap) {
        // 描画にはCSSピクセル寸法（DIP）を使用する。DPIスケーリングを含む
        // ビットマップピクセル寸法は使用しない。
        float draw_w = w.current_request.css_width;
        float draw_h = w.current_request.css_height;
        if (draw_w <= 0) {
            draw_w = bw;  // フォールバック
        }
        if (draw_h <= 0) {
            draw_h = bh;
        }

        cache_.Insert(code_hash, CachedBitmap{ bitmap, draw_w, draw_h });

        if (w.current_request.diagram_entry) {
            w.current_request.diagram_entry->bitmap = bitmap;
            w.current_request.diagram_entry->width = draw_w;
            w.current_request.diagram_entry->height = draw_h;
        }
        if (w.current_request.layout_entry) {
            w.current_request.layout_entry->height = draw_h;
            w.current_request.layout_entry->layout_dirty = false;
        }

        if (file_cache_ && w.current_request.node) {
            const uint64_t fkey = mermaid_util::NodeDiagramHash(*w.current_request.node, w.current_request.max_width, w.current_request.dark_mode);
            auto png_bytes = stream_util::ReadAllBytes(png_stream);
            if (!png_bytes.empty()) {
                file_cache_->StoreAsync(fkey, draw_w, draw_h, std::move(png_bytes));
            }
        }
    }

    FinishWorkerRequest(w);
}

HRESULT MermaidRenderer::CreateBitmapFromPngStream(IStream* stream, ID2D1Bitmap** bitmap,
    float* width, float* height)
{
    if (!stream || !bitmap || !width || !height) {
        return E_FAIL;
    }

    const LARGE_INTEGER zero = {};
    stream->Seek(zero, STREAM_SEEK_SET, nullptr);

    auto created = wic_util::CreateD2DBitmapFromStream(wic_factory_.Get(), render_target_, stream);
    if (!created) {
        return E_FAIL;
    }

    *bitmap = created->bitmap.Detach();
    *width = static_cast<float>(created->pixel_width);
    *height = static_cast<float>(created->pixel_height);
    return S_OK;
}
