#include "mermaid.h"
#include "mermaid_file_cache.h"
#include "mermaid_util.h"
#include "utility.h"
#include "resource.h"
#include <shlwapi.h>
#include <shlobj.h>
#include <wrl/event.h>
#include <filesystem>
#include <functional>

#pragma comment(lib, "windowscodecs.lib")
#pragma comment(lib, "shlwapi.lib")

static constexpr size_t MAX_CACHE_ENTRIES = 64;

static std::span<const std::byte> LoadMermaidJsGzFromResource()
{
    return LoadRcData(IDR_MERMAID_JS_GZ);
}

// 仮想ホスト経由で配信される小さなHTMLテンプレート。mermaid.jsは別の
// <script src>として読み込まれるため、HTMLは2MBのNavigateToString制限を
// 十分に下回る。両リソースはWebResourceRequestedによりインプロセスで配信される。
static const char kMermaidHtml[] = R"HTML(<!DOCTYPE html>
<html>
<head>
<meta charset="utf-8">
<style>
  html, body { margin: 0; padding: 0; overflow: hidden; }
  body { background: white; }
  body.dark { background: #1e1e1e; }
  #container { display: block; }
</style>
<script>
  let currentTheme = null;
  let renderCount = 0;

  function mermaidReady() {
    return typeof mermaid !== 'undefined' && typeof mermaid.render === 'function';
  }

  async function renderMermaid(code, isDark, maxWidth) {
    try {
      if (!mermaidReady()) {
        return JSON.stringify({ ok: false, error: 'mermaid not loaded' });
      }
      const wantTheme = isDark ? 'dark' : 'default';
      document.body.className = isDark ? 'dark' : '';
      if (currentTheme !== wantTheme) {
        mermaid.initialize({
          startOnLoad: false,
          theme: wantTheme,
          securityLevel: 'strict'
        });
        currentTheme = wantTheme;
      }
      const container = document.getElementById('container');
      container.innerHTML = '';
      // 以前の幅制約をリセット
      document.body.style.width = '';
      container.style.maxWidth = '';
      renderCount++;
      const id = 'mmd-' + renderCount;
      const { svg } = await mermaid.render(id, code);
      container.innerHTML = svg;
      await new Promise(r => requestAnimationFrame(() => requestAnimationFrame(r)));
      const svgEl = container.querySelector('svg');
      if (svgEl) {
        const rect = svgEl.getBoundingClientRect();
        return JSON.stringify({
          width: Math.ceil(rect.width),
          height: Math.ceil(rect.height),
          dpr: window.devicePixelRatio || 1,
          ok: true
        });
      }
      return JSON.stringify({ ok: false, error: 'SVG not found' });
    } catch (e) {
      return JSON.stringify({ ok: false, error: e.message || String(e) });
    }
  }

  // DecompressionStream APIを使ってgzip圧縮されたmermaid.jsを読み込む
  (async function() {
    try {
      const resp = await fetch('https://app.local/mermaid.min.js.gz');
      const ds = new DecompressionStream('gzip');
      const text = await new Response(resp.body.pipeThrough(ds)).text();
      const blob = new Blob([text], {type: 'application/javascript'});
      const url = URL.createObjectURL(blob);
      await new Promise((resolve, reject) => {
        const s = document.createElement('script');
        s.src = url;
        s.onload = resolve;
        s.onerror = reject;
        document.head.appendChild(s);
      });
      URL.revokeObjectURL(url);
      var dpr = window.devicePixelRatio || 1;
      window.chrome.webview.postMessage(
        mermaidReady() ? ('mermaid-ready:' + dpr) : 'mermaid-failed');
    } catch(e) {
      window.chrome.webview.postMessage('mermaid-failed');
    }
  })();
</script>
</head>
<body>
<div id="container"></div>
</body>
</html>
)HTML";

// IStreamから全バイトを読み出す（ファイルキャッシュ保存用）。
static std::vector<uint8_t> ReadAllStreamBytes(IStream* stream)
{
    if (!stream) {
        return {};
    }

    STATSTG stat{};
    if (FAILED(stream->Stat(&stat, STATFLAG_NONAME))) {
        return {};
    }

    const auto size = static_cast<size_t>(stat.cbSize.QuadPart);
    if (size == 0) {
        return {};
    }

    const LARGE_INTEGER zero{};
    stream->Seek(zero, STREAM_SEEK_SET, nullptr);

    std::vector<uint8_t> data(size);
    ULONG read = 0;
    stream->Read(data.data(), static_cast<ULONG>(size), &read);
    data.resize(read);
    return data;
}

// ヘルパー: 指定されたバイトデータのコピーを含むIStreamを作成する。
static Microsoft::WRL::ComPtr<IStream> CreateMemoryStream(const void* data, size_t size)
{
    Microsoft::WRL::ComPtr<IStream> stream;
    if (FAILED(CreateStreamOnHGlobal(nullptr, TRUE, &stream)) || !stream) {
        return nullptr;
    }
    if (size > 0 && data) {
        ULONG written = 0;
        stream->Write(data, static_cast<ULONG>(size), &written);
        const LARGE_INTEGER zero{};
        stream->Seek(zero, STREAM_SEEK_SET, nullptr);
    }
    return stream;
}

// ---- ヘルパー ----

static std::pmr::wstring GetWebView2UserDataFolder()
{
    wchar_t* appdata = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &appdata))) {
        return L"";
    }
    const auto path = std::filesystem::path(appdata) / L"mendo" / L"WebView2Data";
    CoTaskMemFree(appdata);
    std::filesystem::create_directories(path);
    return std::pmr::wstring{ path.native() };
}

// オフスクリーンWebView2ホストのウィンドウクラス名
static const wchar_t* kMermaidHostClass = L"mendo_MermaidHost";

// ---- MermaidRendererの実装 ----

int MermaidRenderer::ComputeWorkerCount() noexcept
{
    SYSTEM_INFO si{};
    GetSystemInfo(&si);
    return mermaid_util::ComputeWorkerCount(si.dwNumberOfProcessors);
}

MermaidRenderer::~MermaidRenderer()
{
    for (int i = 0; i < worker_count_; i++) {
        if (workers_[i].controller) {
            workers_[i].controller->Close();
        }
        if (workers_[i].hwnd) {
            DestroyWindow(workers_[i].hwnd);
        }
    }
}

void MermaidRenderer::Init(HWND hwnd, ID2D1RenderTarget* render_target,
    std::function<void()> on_ready)
{
    hwnd_ = hwnd;
    render_target_ = render_target;
    on_all_ready_ = std::move(on_ready);
    worker_count_ = ComputeWorkerCount();

    // PNGデコード用のWICファクトリを作成
    CoCreateInstance(
        CLSID_WICImagingFactory,
        nullptr,
        CLSCTX_INPROC_SERVER,
        IID_PPV_ARGS(&wic_factory_)
    );

    // オフスクリーンでWebView2をホストする非表示ポップアップウィンドウを登録・作成する。
    // WebView2はCapturePreviewでコンテンツをレンダリングするためにIsVisible=TRUEが必要なため、
    // 非表示にする代わりに画面外に配置したポップアップを使用する。
    static bool class_registered = false;
    if (!class_registered) {
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = DefWindowProcW;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = kMermaidHostClass;
        RegisterClassExW(&wc);
        class_registered = true;
    }

    // 各ワーカー用の隠しウィンドウを作成
    for (int i = 0; i < worker_count_; i++) {
        workers_[i].hwnd = CreateWindowExW(
            WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
            kMermaidHostClass,
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

    const std::pmr::wstring user_data = GetWebView2UserDataFolder();

    // 共有WebView2環境を作成し、各ワーカーのコントローラーを初期化する
    CreateCoreWebView2EnvironmentWithOptions(
        nullptr, user_data.c_str(), nullptr,
        Microsoft::WRL::Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>(
            [this](HRESULT result, ICoreWebView2Environment* env) -> HRESULT {
        if (FAILED(result) || !env) {
            return S_OK;
        }
        webview_env_ = env;

        // 全ワーカーのコントローラーを並行して作成する
        for (int i = 0; i < worker_count_; i++) {
            SetupWorker(i);
        }
        return S_OK;
    }).Get());
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
        controller->get_CoreWebView2(&w.webview);

        // ホストウィンドウをWebViewで埋める
        const RECT bounds = { 0, 0, 4096, 4096 };
        controller->put_Bounds(bounds);

        // 不要な機能を無効化する
        Microsoft::WRL::ComPtr<ICoreWebView2Settings> settings;
        w.webview->get_Settings(&settings);
        if (settings) {
            settings->put_AreDevToolsEnabled(FALSE);
            settings->put_IsStatusBarEnabled(FALSE);
            settings->put_AreDefaultContextMenusEnabled(FALSE);
            settings->put_AreDefaultScriptDialogsEnabled(FALSE);
        }

        // Webメッセージをリッスンする
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
                    // 最初のワーカーが準備完了した時点でon_readyを呼び出す。
                    // 残りのワーカーは準備でき次第プールに参加する。
                    if (!ready_) {
                        ready_ = true;
                        if (on_all_ready_) {
                            auto cb = std::move(on_all_ready_);
                            cb();
                        }
                    }
                    ProcessQueue();
                }
                else if (wcsncmp(msg, L"render-result:", 14) == 0) {
                    // "render-result:<id>:<json>" からリクエストIDを解析
                    wchar_t* end = nullptr;
                    const auto id = static_cast<unsigned int>(std::wcstoul(msg + 14, &end, 10));
                    if (end && *end == L':' && id == w.current_request.request_id) {
                        OnRenderResult(index, std::wstring_view(end + 1));
                    }
                }
                else if (wcsncmp(msg, L"capture-ready:", 14) == 0) {
                    // "capture-ready:<id>" からリクエストIDを解析
                    const auto id = static_cast<unsigned int>(std::wcstoul(msg + 14, nullptr, 10));
                    if (id == w.current_request.request_id) {
                        DoCapturePreview(index);
                    }
                }
                else if (wcsncmp(msg, L"render-error:", 13) == 0) {
                    // "render-error:<id>:<message>" からリクエストIDを解析
                    wchar_t* end = nullptr;
                    const auto id = static_cast<unsigned int>(std::wcstoul(msg + 13, &end, 10));
                    if (id == w.current_request.request_id) {
                        FinishWorkerRequest(w);
                    }
                }
                CoTaskMemFree(msg);
            }
            return S_OK;
        }).Get(),
            nullptr);

        // ナビゲーションを制限: app.local以外へのナビゲーションをブロック
        w.webview->add_NavigationStarting(
            Microsoft::WRL::Callback<ICoreWebView2NavigationStartingEventHandler>(
                [](ICoreWebView2*, ICoreWebView2NavigationStartingEventArgs* args) static->HRESULT {
            LPWSTR uri = nullptr;
            if (SUCCEEDED(args->get_Uri(&uri)) && uri) {
                if (wcsncmp(uri, L"https://app.local/", 18) != 0 &&
                    wcscmp(uri, L"about:blank") != 0) {
                    args->put_Cancel(TRUE);
                }
                CoTaskMemFree(uri);
            }
            return S_OK;
        }).Get(),
            nullptr);

        // 新規ウィンドウの要求を全てブロック
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

            // app.local以外へのリクエストをブロック（fetch/XHR等）
            if (url.find(L"app.local") == std::pmr::wstring::npos) {
                Microsoft::WRL::ComPtr<ICoreWebView2WebResourceResponse> response;
                webview_env_->CreateWebResourceResponse(nullptr, 403, L"Blocked", L"", &response);
                args->put_Response(response.Get());
                return S_OK;
            }

            Microsoft::WRL::ComPtr<IStream> stream;
            const wchar_t* headers = nullptr;

            if (url.find(L"/mermaid.min.js.gz") != std::pmr::wstring::npos) {
                // gzip圧縮されたmermaid.js（キャッシュ済み）を配信する。JSがDecompressionStreamで展開する
                if (cached_mermaid_gz_.empty()) {
                    cached_mermaid_gz_ = LoadMermaidJsGzFromResource();
                }
                const auto& gz = cached_mermaid_gz_;
                stream = CreateMemoryStream(gz.data(), gz.size());
                headers = L"Content-Type: application/gzip";
            }
            else {
                // その他のパスにはHTMLテンプレートを配信する
                stream = CreateMemoryStream(kMermaidHtml, sizeof(kMermaidHtml) - 1);
                headers = L"Content-Type: text/html; charset=utf-8";
            }

            if (stream && headers) {
                Microsoft::WRL::ComPtr<ICoreWebView2WebResourceResponse> response;
                webview_env_->CreateWebResourceResponse(stream.Get(), 200, L"OK", headers, &response);
                args->put_Response(response.Get());
            }
            return S_OK;
        }).Get(),
            nullptr);

        // 仮想ホストにナビゲートする（HTML + JSは上記ハンドラにより
        // メモリから配信される）。
        w.webview->Navigate(L"https://app.local/index.html");

        return S_OK;
    }).Get());
}

void MermaidRenderer::SetRenderTarget(ID2D1RenderTarget* render_target)
{
    render_target_ = render_target;
    cache_.clear();
}

void MermaidRenderer::ClearCache()
{
    cache_.clear();
}

void MermaidRenderer::CancelPending()
{
    // 保留キューを空にする
    decltype(pending_requests_) empty;
    pending_requests_.swap(empty);

    // 全ワーカーのレンダリング状態をリセットする。
    // current_requestのrequest_idが0にリセットされるため、
    // 処理中の非同期コールバックはID不一致で自動的に無視される。
    for (int i = 0; i < worker_count_; i++) {
        workers_[i].rendering = false;
        workers_[i].current_request = {};
    }
}

void MermaidRenderer::ClearPendingQueue() noexcept
{
    decltype(pending_requests_) empty;
    pending_requests_.swap(empty);
}

uint64_t MermaidRenderer::HashCode(std::wstring_view code, float max_width, bool dark_mode) const noexcept
{
    return mermaid_util::HashCode(code, max_width, dark_mode);
}

void MermaidRenderer::RequestRender(Node& node, NodeLayoutEntry& layout_entry,
    DiagramEntry& diagram_entry,
    float max_width, bool dark_mode,
    Callback on_complete, void* user_data)
{
    if (node.code_language != SyntaxLanguage::Mermaid) {
        return;
    }

    const auto hash = HashCode(node.text, max_width, dark_mode);

    // まずメモリキャッシュを確認
    const auto it = cache_.find(hash);
    if (it != cache_.end()) {
        diagram_entry.bitmap = it->second.bitmap;
        diagram_entry.width = it->second.width;
        diagram_entry.height = it->second.height;
        layout_entry.height = it->second.height;
        layout_entry.layout_dirty = false;
        if (on_complete) {
            on_complete(user_data);
        }
        return;
    }

    // ファイルキャッシュを確認
    if (file_cache_) {
        MermaidFileCache::CacheEntry fentry;
        std::vector<uint8_t> png_data;
        if (file_cache_->Lookup(hash, fentry, png_data)) {
            auto stream = CreateMemoryStream(png_data.data(), png_data.size());
            if (stream) {
                Microsoft::WRL::ComPtr<ID2D1Bitmap> bitmap;
                float bw = 0, bh = 0;
                if (SUCCEEDED(CreateBitmapFromPngStream(stream.Get(), &bitmap, &bw, &bh)) && bitmap) {
                    diagram_entry.bitmap = bitmap;
                    diagram_entry.width = fentry.css_width;
                    diagram_entry.height = fentry.css_height;
                    layout_entry.height = fentry.css_height;
                    layout_entry.layout_dirty = false;

                    // メモリキャッシュにも格納
                    if (cache_.size() < MAX_CACHE_ENTRIES) {
                        CachedBitmap cached;
                        cached.bitmap = bitmap;
                        cached.width = fentry.css_width;
                        cached.height = fentry.css_height;
                        cache_[hash] = cached;
                    }

                    if (on_complete) {
                        on_complete(user_data);
                    }
                    return;
                }
            }
        }
    }

    // WebView2未初期化ならキューには追加しない。
    // on_ready コールバックで RequestMermaidRenders が再度呼ばれるため、
    // その時点でキャッシュミス分がキューに入る。
    if (!ready_) {
        return;
    }

    // リクエストをキューに追加
    RenderRequest req;
    req.node = &node;
    req.layout_entry = &layout_entry;
    req.diagram_entry = &diagram_entry;
    req.max_width = max_width;
    req.dark_mode = dark_mode;
    req.on_complete = on_complete;
    req.on_complete_data = user_data;
    req.code_hash = hash;
    pending_requests_.push(std::move(req));

    ProcessQueue();
}

void MermaidRenderer::ProcessQueue()
{
    if (!ready_ || pending_requests_.empty()) {
        return;
    }

    // キューからリクエストを取り出し、キャッシュヒットならWebView2をスキップする。
    // 同一コードの図が複数あるとき、最初の1つのレンダリング完了後に
    // 残りをキャッシュから即座に解決できる。
    // アイドル状態のワーカーが見つかれば順次ディスパッチする。
    while (!pending_requests_.empty()) {
        auto& front = pending_requests_.front();
        const auto it = cache_.find(front.code_hash);
        if (it != cache_.end()) {
            // キャッシュヒット: WebView2を経由せずにエントリを更新
            front.diagram_entry->bitmap = it->second.bitmap;
            front.diagram_entry->width = it->second.width;
            front.diagram_entry->height = it->second.height;
            front.layout_entry->height = it->second.height;
            front.layout_entry->layout_dirty = false;
            const auto cb = front.on_complete;
            const auto cb_data = front.on_complete_data;
            pending_requests_.pop();
            if (cb) {
                cb(cb_data);
            }
            continue;
        }

        // アイドル状態のワーカーを探す
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

        // ワーカーにリクエストをディスパッチ
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
    const auto cb = worker.current_request.on_complete;
    const auto cb_data = worker.current_request.on_complete_data;
    worker.current_request = {};
    if (cb) {
        cb(cb_data);
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
    int vp_phys = static_cast<int>(std::ceil(worker.current_request.max_width * worker.dpr));
    if (vp_phys < 1) {
        vp_phys = 1;
    }
    const int h_phys = static_cast<int>(4096 * worker.dpr);
    const RECT bounds = { 0, 0, vp_phys, h_phys };
    worker.controller->put_Bounds(bounds);
    SetWindowPos(worker.hwnd, nullptr, -32000, -32000, vp_phys, h_phys,
        SWP_NOZORDER | SWP_NOACTIVATE);

    // Mermaidをレンダリングする（maxWidth=0はCSS制約なし、ビューポートが制約する）
    // リクエストIDをpostMessageに含め、C++側でコールバックとリクエストを照合する
    const auto js = PmrFormat(
        L"renderMermaid('{}', {}, 0)"
        L".then(function(r){{window.chrome.webview.postMessage('render-result:{}:'+r);}})"
        L".catch(function(e){{window.chrome.webview.postMessage('render-error:{}:'+String(e));}})",
        mermaid_util::JsEscape(worker.current_request.node->text),
        worker.current_request.dark_mode ? L"true" : L"false",
        worker.current_request.request_id, worker.current_request.request_id);

    worker.webview->ExecuteScript(js.c_str(), nullptr);
}

void MermaidRenderer::OnRenderResult(int worker_idx, std::wstring_view json)
{
    auto& w = workers_[worker_idx];

    // jsonはrenderMermaidからの生の文字列。例: {"ok":true,"width":400,"height":300}
    // リクエストIDの照合はメッセージハンドラで実施済み
    float dw = 0, dh = 0;
    bool ok = false;

    const auto find_num = [](std::wstring_view json, std::wstring_view key) static -> float {
        auto pos = json.find(key);
        if (pos == std::wstring_view::npos) {
            return 0;
        }
        pos += key.size();
        while (pos < json.size() && (json[pos] == L':' || json[pos] == L' ')) {
            pos++;
        }
        if (pos >= json.size()) {
            return 0.0f;
        }
        // wstring_viewはnull終端が保証されないため、数値部分を切り出してからwcstofに渡す
        wchar_t buf[64];
        const auto num_len = (std::min)(json.size() - pos, std::size(buf) - 1);
        std::char_traits<wchar_t>::copy(buf, json.data() + pos, num_len);
        buf[num_len] = L'\0';
        return std::wcstof(buf, nullptr);
    };

    dw = find_num(json, L"\"width\"");
    dh = find_num(json, L"\"height\"");
    float dpr = find_num(json, L"\"dpr\"");
    if (dpr <= 0) {
        dpr = 1.0f;
    }
    ok = json.find(L"\"ok\":true") != std::wstring_view::npos
        || json.find(L"\"ok\": true") != std::wstring_view::npos;

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

    // ホストポップアップウィンドウも同じサイズにリサイズする
    SetWindowPos(w.hwnd, nullptr, -32000, -32000, cw, ch,
        SWP_NOZORDER | SWP_NOACTIVATE);

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
    Microsoft::WRL::ComPtr<IStream> pngStream;
    CreateStreamOnHGlobal(nullptr, TRUE, &pngStream);

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

        // キャッシュに格納（エントリ数上限を超えたら任意のエントリを削除）
        if (cache_.size() >= MAX_CACHE_ENTRIES) {
            cache_.erase(cache_.begin());
        }
        CachedBitmap cached;
        cached.bitmap = bitmap;
        cached.width = draw_w;
        cached.height = draw_h;
        cache_[code_hash] = cached;

        // レイアウト/ダイアグラムエントリを更新
        if (w.current_request.diagram_entry) {
            w.current_request.diagram_entry->bitmap = bitmap;
            w.current_request.diagram_entry->width = draw_w;
            w.current_request.diagram_entry->height = draw_h;
        }
        if (w.current_request.layout_entry) {
            w.current_request.layout_entry->height = draw_h;
            w.current_request.layout_entry->layout_dirty = false;
        }

        // ファイルキャッシュに非同期で保存
        if (file_cache_ && w.current_request.node) {
            const uint64_t fkey = HashCode(
                w.current_request.node->text, w.current_request.max_width, w.current_request.dark_mode);
            auto png_bytes = ReadAllStreamBytes(png_stream);
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
    if (!wic_factory_ || !render_target_ || !stream) {
        return E_FAIL;
    }

    // 先頭にシーク
    const LARGE_INTEGER zero = {};
    stream->Seek(zero, STREAM_SEEK_SET, nullptr);

    Microsoft::WRL::ComPtr<IWICBitmapDecoder> decoder;
    HRESULT hr = wic_factory_->CreateDecoderFromStream(
        stream, nullptr, WICDecodeMetadataCacheOnLoad, &decoder);
    if (FAILED(hr)) {
        return hr;
    }

    Microsoft::WRL::ComPtr<IWICBitmapFrameDecode> frame;
    hr = decoder->GetFrame(0, &frame);
    if (FAILED(hr)) {
        return hr;
    }

    Microsoft::WRL::ComPtr<IWICFormatConverter> converter;
    hr = wic_factory_->CreateFormatConverter(&converter);
    if (FAILED(hr)) {
        return hr;
    }

    hr = converter->Initialize(
        frame.Get(), GUID_WICPixelFormat32bppPBGRA,
        WICBitmapDitherTypeNone, nullptr, 0.0f,
        WICBitmapPaletteTypeCustom);
    if (FAILED(hr)) {
        return hr;
    }

    hr = render_target_->CreateBitmapFromWicBitmap(converter.Get(), bitmap);
    if (FAILED(hr)) {
        return hr;
    }

    UINT w = 0, h = 0;
    frame->GetSize(&w, &h);
    *width = static_cast<float>(w);
    *height = static_cast<float>(h);

    return S_OK;
}
