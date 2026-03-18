#include "mermaid.h"
#include "mermaid_util.h"
#include "resource.h"
#include <shlwapi.h>
#include <shlobj.h>
#include <wrl/event.h>
#include <filesystem>
#include <functional>

#pragma comment(lib, "windowscodecs.lib")
#pragma comment(lib, "shlwapi.lib")

// Load the gzip-compressed mermaid.min.js from Win32 resources (RCDATA).
// Returns the raw gzip bytes as a std::string (NOT decompressed).
// WebView2 (Chromium) will decompress it via Content-Encoding: gzip.
static std::string LoadMermaidJsGzFromResource() {
    HMODULE hModule = GetModuleHandleW(nullptr);
    HRSRC hRes = FindResourceW(hModule, MAKEINTRESOURCEW(IDR_MERMAID_JS_GZ), RT_RCDATA);
    if (!hRes) return {};
    HGLOBAL hData = LoadResource(hModule, hRes);
    if (!hData) return {};
    DWORD size = SizeofResource(hModule, hRes);
    const char* data = static_cast<const char*>(LockResource(hData));
    if (!data || size == 0) return {};
    return std::string(data, size);
}

// Small HTML template served via virtual host. mermaid.js is loaded as a
// separate <script src> so the HTML stays well under the 2 MB NavigateToString
// limit.  Both resources are served in-process by WebResourceRequested.
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
      // Reset any previous width constraints
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

  // Load gzip-compressed mermaid.js using DecompressionStream API
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

// Helper: create an IStream containing a copy of the given byte data.
static IStream* CreateMemoryStream(const void* data, size_t size) {
    IStream* stream = nullptr;
    if (FAILED(CreateStreamOnHGlobal(nullptr, TRUE, &stream)) || !stream)
        return nullptr;
    if (size > 0 && data) {
        ULONG written = 0;
        stream->Write(data, static_cast<ULONG>(size), &written);
        LARGE_INTEGER zero{};
        stream->Seek(zero, STREAM_SEEK_SET, nullptr);
    }
    return stream;
}

// ---- Helpers ----

static std::wstring GetWebView2UserDataFolder() {
    wchar_t* appdata = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &appdata))) {
        return L"";
    }
    std::wstring path = std::wstring(appdata) + L"\\mendo\\WebView2Data";
    CoTaskMemFree(appdata);
    std::filesystem::create_directories(path);
    return path;
}

// FNV-1a 64-bit hash - now in mermaid_util.cpp
// JsEscape - now in mermaid_util.cpp

// Window class name for the offscreen WebView2 host
static const wchar_t* kMermaidHostClass = L"mendo_MermaidHost";

// ---- MermaidRenderer implementation ----

MermaidRenderer::~MermaidRenderer() {
    if (webview_controller_) {
        webview_controller_->Close();
    }
    if (webview_hwnd_) {
        DestroyWindow(webview_hwnd_);
    }
}

void MermaidRenderer::Init(HWND hwnd, ID2D1RenderTarget* render_target,
                           std::function<void()> on_ready) {
    hwnd_ = hwnd;
    render_target_ = render_target;

    // Create WIC factory for PNG decoding
    CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                     IID_PPV_ARGS(&wic_factory_));

    // Register and create a hidden popup window to host WebView2 offscreen.
    // WebView2 requires IsVisible=TRUE to render content for CapturePreview,
    // so we use a popup positioned far off-screen instead of hiding.
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
        -32000, -32000,     // Far off-screen
        4096, 4096,         // Large enough for any diagram
        nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);

    if (!webview_hwnd_) {
        OutputDebugStringW(L"[mendo/Mermaid] Failed to create offscreen host window\n");
        return;
    }

    // Show the popup (required for WebView2 to consider it "visible")
    // It's off-screen so the user won't see it
    ShowWindow(webview_hwnd_, SW_SHOWNOACTIVATE);

    std::wstring user_data = GetWebView2UserDataFolder();

    // Create WebView2 environment
    CreateCoreWebView2EnvironmentWithOptions(
        nullptr, user_data.c_str(), nullptr,
        Microsoft::WRL::Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>(
            [this, on_ready](HRESULT result, ICoreWebView2Environment* env) -> HRESULT {
                if (FAILED(result) || !env) {
                    OutputDebugStringW(L"[mendo/Mermaid] WebView2 environment creation failed\n");
                    return S_OK;
                }
                OutputDebugStringW(L"[mendo/Mermaid] WebView2 environment created\n");

                webview_env_ = env;
                env->CreateCoreWebView2Controller(
                    webview_hwnd_,
                    Microsoft::WRL::Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
                        [this, on_ready](HRESULT result, ICoreWebView2Controller* controller) -> HRESULT {
                            if (FAILED(result) || !controller) {
                                OutputDebugStringW(L"[mendo/Mermaid] WebView2 controller creation failed\n");
                                return S_OK;
                            }
                            OutputDebugStringW(L"[mendo/Mermaid] WebView2 controller created\n");

                            webview_controller_ = controller;
                            controller->get_CoreWebView2(&webview_);

                            // Fill the host window with WebView
                            RECT bounds = {0, 0, 4096, 4096};
                            controller->put_Bounds(bounds);

                            // Disable features we don't need
                            ComPtr<ICoreWebView2Settings> settings;
                            webview_->get_Settings(&settings);
                            if (settings) {
                                settings->put_AreDevToolsEnabled(FALSE);
                                settings->put_IsStatusBarEnabled(FALSE);
                                settings->put_AreDefaultContextMenusEnabled(FALSE);
                                settings->put_AreDefaultScriptDialogsEnabled(FALSE);
                            }

                            // Listen for web messages (mermaid-ready signal)
                            webview_->add_WebMessageReceived(
                                Microsoft::WRL::Callback<ICoreWebView2WebMessageReceivedEventHandler>(
                                    [this, on_ready](ICoreWebView2*,
                                                     ICoreWebView2WebMessageReceivedEventArgs* args) -> HRESULT {
                                        LPWSTR msg = nullptr;
                                        if (SUCCEEDED(args->TryGetWebMessageAsString(&msg)) && msg) {
                                            if (wcsncmp(msg, L"mermaid-ready:", 14) == 0) {
                                                // Parse DPR from "mermaid-ready:<dpr>"
                                                float dpr = 1.0f;
                                                try { dpr = std::stof(std::wstring(msg + 14)); }
                                                catch (...) {}
                                                if (dpr > 0) dpr_ = dpr;
                                                {
                                                    std::wstring dbg = L"[mendo/Mermaid] Received mermaid-ready dpr="
                                                        + std::to_wstring(dpr_) + L"\n";
                                                    OutputDebugStringW(dbg.c_str());
                                                }
                                                ready_ = true;
                                                if (on_ready) on_ready();
                                                ProcessQueue();
                                            } else if (wcsncmp(msg, L"render-result:", 14) == 0) {
                                                OnMermaidRenderResult(std::wstring(msg + 14));
                                            } else if (wcscmp(msg, L"capture-ready") == 0) {
                                                DoCapturePreview();
                                            } else if (wcsncmp(msg, L"render-error:", 13) == 0) {
                                                OutputDebugStringW(L"[mendo/Mermaid] Render error via postMessage: ");
                                                OutputDebugStringW(msg + 13);
                                                OutputDebugStringW(L"\n");
                                                FinishCurrentRequest();
                                            } else {
                                                OutputDebugStringW(L"[mendo/Mermaid] Received message: ");
                                                OutputDebugStringW(msg);
                                                OutputDebugStringW(L"\n");
                                            }
                                            CoTaskMemFree(msg);
                                        }
                                        return S_OK;
                                    }).Get(),
                                nullptr);

                            // Intercept requests to the virtual host and serve
                            // HTML / mermaid.js from embedded Win32 resources.
                            webview_->AddWebResourceRequestedFilter(
                                L"https://app.local/*",
                                COREWEBVIEW2_WEB_RESOURCE_CONTEXT_ALL);

                            webview_->add_WebResourceRequested(
                                Microsoft::WRL::Callback<ICoreWebView2WebResourceRequestedEventHandler>(
                                    [this](ICoreWebView2*,
                                           ICoreWebView2WebResourceRequestedEventArgs* args) -> HRESULT {
                                        ComPtr<ICoreWebView2WebResourceRequest> request;
                                        args->get_Request(&request);
                                        LPWSTR uri = nullptr;
                                        request->get_Uri(&uri);
                                        std::wstring url(uri ? uri : L"");
                                        CoTaskMemFree(uri);

                                        IStream* stream = nullptr;
                                        const wchar_t* headers = nullptr;

                                        if (url.find(L"/mermaid.min.js.gz") != std::wstring::npos) {
                                            // Serve gzip-compressed mermaid.js (cached); JS decompresses via DecompressionStream
                                            if (cached_mermaid_gz_.empty()) {
                                                cached_mermaid_gz_ = LoadMermaidJsGzFromResource();
                                            }
                                            const auto& gz = cached_mermaid_gz_;
                                            OutputDebugStringW(L"[mendo/Mermaid] Serving mermaid.min.js.gz from resource (");
                                            OutputDebugStringW(std::to_wstring(gz.size()).c_str());
                                            OutputDebugStringW(L" bytes, gzip)\n");
                                            stream = CreateMemoryStream(gz.data(), gz.size());
                                            headers = L"Content-Type: application/gzip";
                                        } else {
                                            // Serve the HTML template for any other path
                                            stream = CreateMemoryStream(kMermaidHtml, sizeof(kMermaidHtml) - 1);
                                            headers = L"Content-Type: text/html; charset=utf-8";
                                        }

                                        if (stream && headers) {
                                            ComPtr<ICoreWebView2WebResourceResponse> response;
                                            webview_env_->CreateWebResourceResponse(
                                                stream, 200, L"OK", headers, &response);
                                            args->put_Response(response.Get());
                                            stream->Release();
                                        }
                                        return S_OK;
                                    }).Get(),
                                nullptr);

                            // Navigate to the virtual host (HTML + JS served
                            // from memory by the handler above).
                            webview_->Navigate(L"https://app.local/index.html");

                            return S_OK;
                        }).Get());
                return S_OK;
            }).Get());
}

void MermaidRenderer::SetRenderTarget(ID2D1RenderTarget* render_target) {
    render_target_ = render_target;
    cache_.clear();
}

void MermaidRenderer::ClearCache() {
    cache_.clear();
}

void MermaidRenderer::CancelPending() {
    // Drain the pending queue
    std::queue<RenderRequest> empty;
    pending_requests_.swap(empty);

    // Reset rendering state so new requests can be processed
    // after in-flight callbacks complete harmlessly.
    rendering_ = false;
    current_request_ = {};
}

std::wstring MermaidRenderer::HashCode(const std::wstring& code, float max_width, bool dark_mode) const {
    std::wstring key = code + L"|" + std::to_wstring(static_cast<int>(max_width))
                       + L"|" + (dark_mode ? L"d" : L"l");
    return mermaid_util::SimpleHash(key);
}

void MermaidRenderer::RequestRender(Node& node, NodeLayoutEntry& layout_entry,
                                     DiagramEntry& diagram_entry,
                                     float max_width, bool dark_mode,
                                     std::function<void()> on_complete) {
    if (node.code_language != SyntaxLanguage::Mermaid) return;

    std::wstring hash = HashCode(node.text, max_width, dark_mode);

    // Check cache first
    auto it = cache_.find(hash);
    if (it != cache_.end()) {
        diagram_entry.bitmap = it->second.bitmap;
        diagram_entry.width = it->second.width;
        diagram_entry.height = it->second.height;
        layout_entry.height = it->second.height;
        layout_entry.layout_dirty = false;
        if (on_complete) on_complete();
        return;
    }

    // Queue the request
    RenderRequest req;
    req.node = &node;
    req.layout_entry = &layout_entry;
    req.diagram_entry = &diagram_entry;
    req.max_width = max_width;
    req.dark_mode = dark_mode;
    req.on_complete = std::move(on_complete);
    req.code_hash = hash;
    pending_requests_.push(std::move(req));

    if (!rendering_) {
        ProcessQueue();
    }
}

void MermaidRenderer::ProcessQueue() {
    if (!ready_ || rendering_ || pending_requests_.empty()) return;

    current_request_ = std::move(pending_requests_.front());
    pending_requests_.pop();
    rendering_ = true;

    RenderMermaidInWebView(current_request_.node->text,
                           current_request_.max_width,
                           current_request_.dark_mode);
}

void MermaidRenderer::FinishCurrentRequest() {
    rendering_ = false;
    auto cb = std::move(current_request_.on_complete);
    current_request_ = {};
    if (cb) cb();
    ProcessQueue();
}

void MermaidRenderer::RenderMermaidInWebView(const std::wstring& code, float max_width, bool dark_mode) {
    if (!webview_) {
        FinishCurrentRequest();
        return;
    }

    // Set WebView2 bounds so that the CSS viewport equals max_width (DIPs).
    // Bounds are in physical pixels of the popup window, and WebView2
    // internally divides by devicePixelRatio to get CSS viewport size.
    int vp_phys = static_cast<int>(std::ceil(max_width * dpr_));
    if (vp_phys < 1) vp_phys = 1;
    int h_phys = static_cast<int>(4096 * dpr_);
    RECT bounds = {0, 0, vp_phys, h_phys};
    webview_controller_->put_Bounds(bounds);
    SetWindowPos(webview_hwnd_, nullptr, -32000, -32000, vp_phys, h_phys,
                 SWP_NOZORDER | SWP_NOACTIVATE);

    {
        std::wstring dbg = L"[mendo/Mermaid] Render: max_width=" + std::to_wstring(static_cast<int>(max_width))
            + L" dpr=" + std::to_wstring(dpr_)
            + L" bounds_w=" + std::to_wstring(vp_phys) + L"\n";
        OutputDebugStringW(dbg.c_str());
    }

    // Render mermaid (maxWidth=0 means no CSS constraint, viewport constrains)
    std::wstring js = L"renderMermaid('" + mermaid_util::JsEscape(code) + L"', "
                      + (dark_mode ? L"true" : L"false") + L", 0"
                      + L").then(function(r){window.chrome.webview.postMessage('render-result:'+r);"
                        L"}).catch(function(e){window.chrome.webview.postMessage('render-error:'+String(e));})";

    webview_->ExecuteScript(js.c_str(), nullptr);
}

void MermaidRenderer::OnMermaidRenderResult(const std::wstring& json) {
    // json is the raw string from renderMermaid, e.g. {"ok":true,"width":400,"height":300}
    float dw = 0, dh = 0;
    bool ok = false;

    auto find_num = [&](const std::wstring& key) -> float {
        auto pos = json.find(key);
        if (pos == std::wstring::npos) return 0;
        pos += key.size();
        while (pos < json.size() && (json[pos] == L':' || json[pos] == L' ')) pos++;
        std::wstring num;
        while (pos < json.size() && (iswdigit(json[pos]) || json[pos] == L'.')) {
            num += json[pos++];
        }
        if (num.empty()) return 0.0f;
        try { return std::stof(num); } catch (...) { return 0.0f; }
    };

    dw = find_num(L"\"width\"");
    dh = find_num(L"\"height\"");
    float dpr = find_num(L"\"dpr\"");
    if (dpr <= 0) dpr = 1.0f;
    ok = json.find(L"\"ok\":true") != std::wstring::npos
         || json.find(L"\"ok\": true") != std::wstring::npos;

    if (!ok || dw <= 0 || dh <= 0) {
        OutputDebugStringW(L"[mendo/Mermaid] renderMermaid returned error or zero size\n");
        OutputDebugStringW((L"[mendo/Mermaid]   result: " + json + L"\n").c_str());
        FinishCurrentRequest();
        return;
    }
    {
        std::wstring msg = L"[mendo/Mermaid] renderMermaid ok: "
            + std::to_wstring(static_cast<int>(dw)) + L"x"
            + std::to_wstring(static_cast<int>(dh))
            + L" dpr=" + std::to_wstring(dpr) + L"\n";
        OutputDebugStringW(msg.c_str());
    }

    // Store CSS pixel dimensions for later use as drawing size (DIPs)
    current_request_.css_width = dw;
    current_request_.css_height = dh;
    current_request_.dpr = dpr;

    // Resize WebView to exact diagram size for capture.
    // Multiply CSS pixels by devicePixelRatio to get physical pixels.
    int cw = static_cast<int>(std::ceil(dw * dpr));
    int ch = static_cast<int>(std::ceil(dh * dpr));
    RECT capBounds = {0, 0, static_cast<LONG>(cw), static_cast<LONG>(ch)};
    webview_controller_->put_Bounds(capBounds);

    // Also resize the host popup window to match
    SetWindowPos(webview_hwnd_, nullptr, -32000, -32000, cw, ch,
                 SWP_NOZORDER | SWP_NOACTIVATE);

    // Wait for the WebView to re-render at the new size using rAF,
    // then signal via postMessage (avoids Promise-await issue).
    webview_->ExecuteScript(
        L"requestAnimationFrame(function(){requestAnimationFrame(function(){"
        L"window.chrome.webview.postMessage('capture-ready');});})",
        nullptr);
}

void MermaidRenderer::DoCapturePreview() {
    if (!webview_) {
        FinishCurrentRequest();
        return;
    }

    IStream* pngStream = nullptr;
    CreateStreamOnHGlobal(nullptr, TRUE, &pngStream);

    HRESULT hr = webview_->CapturePreview(
        COREWEBVIEW2_CAPTURE_PREVIEW_IMAGE_FORMAT_PNG,
        pngStream,
        Microsoft::WRL::Callback<ICoreWebView2CapturePreviewCompletedHandler>(
            [this, pngStream](HRESULT hr3) -> HRESULT {
                if (SUCCEEDED(hr3) && pngStream) {
                    OnCaptureComplete(current_request_.code_hash, pngStream);
                } else {
                    FinishCurrentRequest();
                }
                if (pngStream) pngStream->Release();
                return S_OK;
            }).Get());

    if (FAILED(hr)) {
        if (pngStream) pngStream->Release();
        FinishCurrentRequest();
    }
}

void MermaidRenderer::OnCaptureComplete(const std::wstring& code_hash, IStream* png_stream) {
    ComPtr<ID2D1Bitmap> bitmap;
    float bw = 0, bh = 0;

    if (SUCCEEDED(CreateBitmapFromPngStream(png_stream, &bitmap, &bw, &bh)) && bitmap) {
        // Use CSS pixel dimensions (DIPs) for drawing, not bitmap pixel
        // dimensions which include DPI scaling.
        float draw_w = current_request_.css_width;
        float draw_h = current_request_.css_height;
        if (draw_w <= 0) draw_w = bw;  // fallback
        if (draw_h <= 0) draw_h = bh;

        {
            std::wstring msg = L"[mendo/Mermaid] Capture: bitmap="
                + std::to_wstring(static_cast<int>(bw)) + L"x"
                + std::to_wstring(static_cast<int>(bh))
                + L" draw=" + std::to_wstring(static_cast<int>(draw_w))
                + L"x" + std::to_wstring(static_cast<int>(draw_h)) + L"\n";
            OutputDebugStringW(msg.c_str());
        }

        // Store in cache
        CachedBitmap cached;
        cached.bitmap = bitmap;
        cached.width = draw_w;
        cached.height = draw_h;
        cache_[code_hash] = cached;

        // Update the layout/diagram entries
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
                                                    float* width, float* height) {
    if (!wic_factory_ || !render_target_ || !stream) return E_FAIL;

    // Seek to beginning
    LARGE_INTEGER zero = {};
    stream->Seek(zero, STREAM_SEEK_SET, nullptr);

    ComPtr<IWICBitmapDecoder> decoder;
    HRESULT hr = wic_factory_->CreateDecoderFromStream(
        stream, nullptr, WICDecodeMetadataCacheOnLoad, &decoder);
    if (FAILED(hr)) return hr;

    ComPtr<IWICBitmapFrameDecode> frame;
    hr = decoder->GetFrame(0, &frame);
    if (FAILED(hr)) return hr;

    ComPtr<IWICFormatConverter> converter;
    hr = wic_factory_->CreateFormatConverter(&converter);
    if (FAILED(hr)) return hr;

    hr = converter->Initialize(
        frame.Get(), GUID_WICPixelFormat32bppPBGRA,
        WICBitmapDitherTypeNone, nullptr, 0.0f,
        WICBitmapPaletteTypeCustom);
    if (FAILED(hr)) return hr;

    hr = render_target_->CreateBitmapFromWicBitmap(converter.Get(), bitmap);
    if (FAILED(hr)) return hr;

    UINT w = 0, h = 0;
    frame->GetSize(&w, &h);
    *width = static_cast<float>(w);
    *height = static_cast<float>(h);

    return S_OK;
}
