#include "mermaid.h"
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

// Win32リソース（RCDATA）からgzip圧縮されたmermaid.min.jsのバイト列を取得する。
// リソースはプロセスのアドレス空間にマップされており、コピーせずに直接参照できる。
static std::span<const std::byte> LoadMermaidJsGzFromResource()
{
    HMODULE hModule = GetModuleHandleW(nullptr);
    HRSRC hRes = FindResourceW(hModule, MAKEINTRESOURCEW(IDR_MERMAID_JS_GZ), RT_RCDATA);
    if (!hRes) {
        return {};
    }
    HGLOBAL hData = LoadResource(hModule, hRes);
    if (!hData) {
        return {};
    }
    DWORD size = SizeofResource(hModule, hRes);
    const auto* data = static_cast<const std::byte*>(LockResource(hData));
    if (!data || size == 0) {
        return {};
    }
    return { data, size };
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

// ヘルパー: 指定されたバイトデータのコピーを含むIStreamを作成する。
static ComPtr<IStream> CreateMemoryStream(const void* data, size_t size)
{
    ComPtr<IStream> stream;
    if (FAILED(CreateStreamOnHGlobal(nullptr, TRUE, &stream)) || !stream) {
        return nullptr;
    }
    if (size > 0 && data) {
        ULONG written = 0;
        stream->Write(data, static_cast<ULONG>(size), &written);
        LARGE_INTEGER zero{};
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
    auto path = std::filesystem::path(appdata) / L"mendo" / L"WebView2Data";
    CoTaskMemFree(appdata);
    std::filesystem::create_directories(path);
    return std::pmr::wstring{ std::wstring_view{path.native()} };
}

// FNV-1a 64ビットハッシュ - mermaid_util.cppに移動済み
// JsEscape - mermaid_util.cppに移動済み

// オフスクリーンWebView2ホストのウィンドウクラス名
static const wchar_t* kMermaidHostClass = L"mendo_MermaidHost";

// ---- MermaidRendererの実装 ----

MermaidRenderer::~MermaidRenderer()
{
    if (webview_controller_) {
        webview_controller_->Close();
    }
    if (webview_hwnd_) {
        DestroyWindow(webview_hwnd_);
    }
}

void MermaidRenderer::Init(HWND hwnd, ID2D1RenderTarget* render_target,
    std::function<void()> on_ready)
{
    hwnd_ = hwnd;
    render_target_ = render_target;

    // PNGデコード用のWICファクトリを作成
    CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
        IID_PPV_ARGS(&wic_factory_));

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

    webview_hwnd_ = CreateWindowExW(
        WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
        kMermaidHostClass,
        L"",
        WS_POPUP,
        -32000, -32000,     // 画面外の遠い位置
        4096, 4096,         // どのダイアグラムにも十分な大きさ
        nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);

    if (!webview_hwnd_) {
        return;
    }

    // ポップアップを表示する（WebView2が「可視」と認識するために必要）
    // 画面外にあるためユーザーには見えない
    ShowWindow(webview_hwnd_, SW_SHOWNOACTIVATE);

    std::pmr::wstring user_data = GetWebView2UserDataFolder();

    // WebView2環境を作成
    CreateCoreWebView2EnvironmentWithOptions(
        nullptr, user_data.c_str(), nullptr,
        Microsoft::WRL::Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>(
            [this, on_ready](HRESULT result, ICoreWebView2Environment* env) -> HRESULT {
        if (FAILED(result) || !env) {
            return S_OK;
        }
        webview_env_ = env;
        env->CreateCoreWebView2Controller(
            webview_hwnd_,
            Microsoft::WRL::Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
                [this, on_ready](HRESULT result, ICoreWebView2Controller* controller) -> HRESULT {
            if (FAILED(result) || !controller) {
                return S_OK;
            }

            webview_controller_ = controller;
            controller->get_CoreWebView2(&webview_);

            // ホストウィンドウをWebViewで埋める
            RECT bounds = { 0, 0, 4096, 4096 };
            controller->put_Bounds(bounds);

            // 不要な機能を無効化する
            ComPtr<ICoreWebView2Settings> settings;
            webview_->get_Settings(&settings);
            if (settings) {
                settings->put_AreDevToolsEnabled(FALSE);
                settings->put_IsStatusBarEnabled(FALSE);
                settings->put_AreDefaultContextMenusEnabled(FALSE);
                settings->put_AreDefaultScriptDialogsEnabled(FALSE);
            }

            // Webメッセージ（mermaid-readyシグナル）をリッスンする
            webview_->add_WebMessageReceived(
                Microsoft::WRL::Callback<ICoreWebView2WebMessageReceivedEventHandler>(
                    [this, on_ready](ICoreWebView2*,
                        ICoreWebView2WebMessageReceivedEventArgs* args) -> HRESULT {
                LPWSTR msg = nullptr;
                if (SUCCEEDED(args->TryGetWebMessageAsString(&msg)) && msg) {
                    if (wcsncmp(msg, L"mermaid-ready:", 14) == 0) {
                        // "mermaid-ready:<dpr>"からDPRを解析
                        float dpr = std::wcstof(msg + 14, nullptr);
                        if (dpr > 0) {
                            dpr_ = dpr;
                        }
                        ready_ = true;
                        if (on_ready) {
                            on_ready();
                        }
                        ProcessQueue();
                    }
                    else if (wcsncmp(msg, L"render-result:", 14) == 0) {
                        // "render-result:<id>:<json>" からリクエストIDを解析
                        wchar_t* end = nullptr;
                        auto id = static_cast<unsigned int>(std::wcstoul(msg + 14, &end, 10));
                        if (end && *end == L':' && id == current_request_.request_id) {
                            OnMermaidRenderResult(std::wstring_view(end + 1));
                        }
                    }
                    else if (wcsncmp(msg, L"capture-ready:", 14) == 0) {
                        // "capture-ready:<id>" からリクエストIDを解析
                        auto id = static_cast<unsigned int>(std::wcstoul(msg + 14, nullptr, 10));
                        if (id == current_request_.request_id) {
                            DoCapturePreview();
                        }
                    }
                    else if (wcsncmp(msg, L"render-error:", 13) == 0) {
                        // "render-error:<id>:<message>" からリクエストIDを解析
                        wchar_t* end = nullptr;
                        auto id = static_cast<unsigned int>(std::wcstoul(msg + 13, &end, 10));
                        if (id == current_request_.request_id) {
                            FinishCurrentRequest();
                        }
                    }
                    else {
                    }
                    CoTaskMemFree(msg);
                }
                return S_OK;
            }).Get(),
                nullptr);

            // ナビゲーションを制限: app.local以外へのナビゲーションをブロック
            webview_->add_NavigationStarting(
                Microsoft::WRL::Callback<ICoreWebView2NavigationStartingEventHandler>(
                    [](ICoreWebView2*, ICoreWebView2NavigationStartingEventArgs* args) -> HRESULT {
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
            webview_->add_NewWindowRequested(
                Microsoft::WRL::Callback<ICoreWebView2NewWindowRequestedEventHandler>(
                    [](ICoreWebView2*, ICoreWebView2NewWindowRequestedEventArgs* args) -> HRESULT {
                args->put_Handled(TRUE);
                return S_OK;
            }).Get(),
                nullptr);

            // 仮想ホストへのリクエストをインターセプトし、
            // 埋め込みWin32リソースからHTML / mermaid.jsを配信する。
            // 全URLをフィルタし、app.local以外へのリクエストもブロックする。
            webview_->AddWebResourceRequestedFilter(
                L"*",
                COREWEBVIEW2_WEB_RESOURCE_CONTEXT_ALL);

            webview_->add_WebResourceRequested(
                Microsoft::WRL::Callback<ICoreWebView2WebResourceRequestedEventHandler>(
                    [this](ICoreWebView2*,
                        ICoreWebView2WebResourceRequestedEventArgs* args) -> HRESULT {
                ComPtr<ICoreWebView2WebResourceRequest> request;
                args->get_Request(&request);
                LPWSTR uri = nullptr;
                request->get_Uri(&uri);
                std::pmr::wstring url(uri ? uri : L"");
                CoTaskMemFree(uri);

                // app.local以外へのリクエストをブロック（fetch/XHR等）
                if (url.find(L"app.local") == std::pmr::wstring::npos) {
                    ComPtr<ICoreWebView2WebResourceResponse> response;
                    webview_env_->CreateWebResourceResponse(
                        nullptr, 403, L"Blocked", L"", &response);
                    args->put_Response(response.Get());
                    return S_OK;
                }

                ComPtr<IStream> stream;
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
                    ComPtr<ICoreWebView2WebResourceResponse> response;
                    webview_env_->CreateWebResourceResponse(
                        stream.Get(), 200, L"OK", headers, &response);
                    args->put_Response(response.Get());
                }
                return S_OK;
            }).Get(),
                nullptr);

            // 仮想ホストにナビゲートする（HTML + JSは上記ハンドラにより
            // メモリから配信される）。
            webview_->Navigate(L"https://app.local/index.html");

            return S_OK;
        }).Get());
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

    // レンダリング状態をリセットし、新しいリクエストを処理できるようにする。
    // current_request_のrequest_idが0にリセットされるため、
    // 処理中の非同期コールバックはID不一致で自動的に無視される。
    rendering_ = false;
    current_request_ = {};
}

void MermaidRenderer::ClearPendingQueue() noexcept
{
    decltype(pending_requests_) empty;
    pending_requests_.swap(empty);
}

uint64_t MermaidRenderer::HashCode(std::wstring_view code, float max_width, bool dark_mode) const
{
    return mermaid_util::CombinedHash(code, static_cast<int>(max_width), dark_mode);
}

void MermaidRenderer::RequestRender(Node& node, NodeLayoutEntry& layout_entry,
    DiagramEntry& diagram_entry,
    float max_width, bool dark_mode,
    Callback on_complete, void* user_data)
{
    if (node.code_language != SyntaxLanguage::Mermaid) {
        return;
    }

    auto hash = HashCode(node.text, max_width, dark_mode);

    // まずキャッシュを確認
    auto it = cache_.find(hash);
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

    // リクエストをキューに追加
    RenderRequest req;
    req.node = &node;
    req.layout_entry = &layout_entry;
    req.diagram_entry = &diagram_entry;
    req.max_width = max_width;
    req.dark_mode = dark_mode;
    req.on_complete = on_complete;
    req.on_complete_data = user_data;
    req.code_hash = std::move(hash);
    pending_requests_.push(std::move(req));

    if (!rendering_) {
        ProcessQueue();
    }
}

void MermaidRenderer::ProcessQueue()
{
    if (!ready_ || rendering_ || pending_requests_.empty()) {
        return;
    }

    current_request_ = std::move(pending_requests_.front());
    pending_requests_.pop();
    current_request_.request_id = ++request_counter_;
    rendering_ = true;

    RenderMermaidInWebView(std::wstring_view{ current_request_.node->text },
        current_request_.max_width,
        current_request_.dark_mode);
}

void MermaidRenderer::FinishCurrentRequest()
{
    rendering_ = false;
    auto cb = current_request_.on_complete;
    auto cb_data = current_request_.on_complete_data;
    current_request_ = {};
    if (cb) {
        cb(cb_data);
    }
    ProcessQueue();
}

void MermaidRenderer::RenderMermaidInWebView(std::wstring_view code, float max_width, bool dark_mode)
{
    if (!webview_) {
        FinishCurrentRequest();
        return;
    }

    // CSSビューポートがmax_width（DIP）と等しくなるようにWebView2の境界を設定する。
    // 境界はポップアップウィンドウの物理ピクセル単位で、WebView2は
    // 内部でdevicePixelRatioで除算してCSSビューポートサイズを求める。
    int vp_phys = static_cast<int>(std::ceil(max_width * dpr_));
    if (vp_phys < 1) {
        vp_phys = 1;
    }
    int h_phys = static_cast<int>(4096 * dpr_);
    RECT bounds = { 0, 0, vp_phys, h_phys };
    webview_controller_->put_Bounds(bounds);
    SetWindowPos(webview_hwnd_, nullptr, -32000, -32000, vp_phys, h_phys,
        SWP_NOZORDER | SWP_NOACTIVATE);

    // Mermaidをレンダリングする（maxWidth=0はCSS制約なし、ビューポートが制約する）
    // リクエストIDをpostMessageに含め、C++側でコールバックとリクエストを照合する
    auto js = PmrFormat(
        L"renderMermaid('{}', {}, 0)"
        L".then(function(r){{window.chrome.webview.postMessage('render-result:{}:'+r);}})"
        L".catch(function(e){{window.chrome.webview.postMessage('render-error:{}:'+String(e));}})",
        mermaid_util::JsEscape(code), dark_mode ? L"true" : L"false",
        current_request_.request_id, current_request_.request_id);

    webview_->ExecuteScript(js.c_str(), nullptr);
}

void MermaidRenderer::OnMermaidRenderResult(std::wstring_view json)
{
    // jsonはrenderMermaidからの生の文字列。例: {"ok":true,"width":400,"height":300}
    // リクエストIDの照合はメッセージハンドラで実施済み
    float dw = 0, dh = 0;
    bool ok = false;

    auto find_num = [](std::wstring_view json, std::wstring_view key) static -> float {
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
        auto num_len = (std::min)(json.size() - pos, std::size(buf) - 1);
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
        FinishCurrentRequest();
        return;
    }

    // 描画サイズ（DIP）として後で使用するためにCSSピクセル寸法を保存する
    current_request_.css_width = dw;
    current_request_.css_height = dh;
    current_request_.dpr = dpr;

    // キャプチャ用にWebViewをダイアグラムの正確なサイズにリサイズする。
    // CSSピクセルにdevicePixelRatioを掛けて物理ピクセルを求める。
    int cw = static_cast<int>(std::ceil(dw * dpr));
    int ch = static_cast<int>(std::ceil(dh * dpr));
    RECT capBounds = { 0, 0, static_cast<LONG>(cw), static_cast<LONG>(ch) };
    webview_controller_->put_Bounds(capBounds);

    // ホストポップアップウィンドウも同じサイズにリサイズする
    SetWindowPos(webview_hwnd_, nullptr, -32000, -32000, cw, ch,
        SWP_NOZORDER | SWP_NOACTIVATE);

    // rAFを使ってWebViewが新しいサイズで再レンダリングするのを待ち、
    // postMessageでシグナルを送る（Promise-awaitの問題を回避する）。
    // リクエストIDを含めて、C++側でコールバックとリクエストを照合する。
    auto cap_js = PmrFormat(
        L"requestAnimationFrame(function(){{requestAnimationFrame(function(){{"
        L"window.chrome.webview.postMessage('capture-ready:{}');}});}});",
        current_request_.request_id);
    webview_->ExecuteScript(cap_js.c_str(), nullptr);
}

void MermaidRenderer::DoCapturePreview()
{
    if (!webview_) {
        FinishCurrentRequest();
        return;
    }

    // CapturePreviewコールバックでもリクエストIDを照合し、
    // CancelPending後に到着した古いキャプチャ結果を無視する
    unsigned int req_id = current_request_.request_id;
    ComPtr<IStream> pngStream;
    CreateStreamOnHGlobal(nullptr, TRUE, &pngStream);

    HRESULT hr = webview_->CapturePreview(
        COREWEBVIEW2_CAPTURE_PREVIEW_IMAGE_FORMAT_PNG,
        pngStream.Get(),
        Microsoft::WRL::Callback<ICoreWebView2CapturePreviewCompletedHandler>(
            [this, pngStream, req_id](HRESULT hr3) -> HRESULT {
        if (current_request_.request_id != req_id) {
            return S_OK;
        }
        if (SUCCEEDED(hr3) && pngStream) {
            OnCaptureComplete(current_request_.code_hash, pngStream.Get());
        }
        else {
            FinishCurrentRequest();
        }
        return S_OK;
    }).Get());

    if (FAILED(hr)) {
        FinishCurrentRequest();
    }
}

void MermaidRenderer::OnCaptureComplete(uint64_t code_hash, IStream* png_stream)
{
    ComPtr<ID2D1Bitmap> bitmap;
    float bw = 0, bh = 0;

    if (SUCCEEDED(CreateBitmapFromPngStream(png_stream, &bitmap, &bw, &bh)) && bitmap) {
        // 描画にはCSSピクセル寸法（DIP）を使用する。DPIスケーリングを含む
        // ビットマップピクセル寸法は使用しない。
        float draw_w = current_request_.css_width;
        float draw_h = current_request_.css_height;
        if (draw_w <= 0) {
            draw_w = bw;  // フォールバック
        }
        if (draw_h <= 0) {
            draw_h = bh;
        }

        // キャッシュに格納（エントリ数上限を超えたら任意のエントリを削除）
        static constexpr size_t MAX_CACHE_ENTRIES = 4096;
        if (cache_.size() >= MAX_CACHE_ENTRIES) {
            cache_.erase(cache_.begin());
        }
        CachedBitmap cached;
        cached.bitmap = bitmap;
        cached.width = draw_w;
        cached.height = draw_h;
        cache_[code_hash] = cached;

        // レイアウト/ダイアグラムエントリを更新
        if (current_request_.diagram_entry) {
            current_request_.diagram_entry->bitmap = bitmap;
            current_request_.diagram_entry->width = draw_w;
            current_request_.diagram_entry->height = draw_h;
        }
        if (current_request_.layout_entry) {
            current_request_.layout_entry->height = draw_h;
            current_request_.layout_entry->layout_dirty = false;
        }
    }

    FinishCurrentRequest();
}

HRESULT MermaidRenderer::CreateBitmapFromPngStream(IStream* stream, ID2D1Bitmap** bitmap,
    float* width, float* height)
{
    if (!wic_factory_ || !render_target_ || !stream) {
        return E_FAIL;
    }

    // 先頭にシーク
    LARGE_INTEGER zero = {};
    stream->Seek(zero, STREAM_SEEK_SET, nullptr);

    ComPtr<IWICBitmapDecoder> decoder;
    HRESULT hr = wic_factory_->CreateDecoderFromStream(
        stream, nullptr, WICDecodeMetadataCacheOnLoad, &decoder);
    if (FAILED(hr)) {
        return hr;
    }

    ComPtr<IWICBitmapFrameDecode> frame;
    hr = decoder->GetFrame(0, &frame);
    if (FAILED(hr)) {
        return hr;
    }

    ComPtr<IWICFormatConverter> converter;
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
