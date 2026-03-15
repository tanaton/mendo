#include "mermaid.h"
#include "resource.h"
#include <shlwapi.h>
#include <shlobj.h>
#include <wrl/event.h>
#include <filesystem>
#include <functional>

#pragma comment(lib, "windowscodecs.lib")
#pragma comment(lib, "shlwapi.lib")

// Load the embedded mermaid.min.js from Win32 resources (RCDATA).
// Returns the JS source as a UTF-8 std::string, or empty on failure.
static std::string LoadMermaidJsFromResource() {
    HMODULE hModule = GetModuleHandleW(nullptr);
    HRSRC hRes = FindResourceW(hModule, MAKEINTRESOURCEW(IDR_MERMAID_JS), RT_RCDATA);
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
  #container { display: inline-block; }
</style>
<script src="https://app.local/mermaid.min.js"></script>
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
      container.style.maxWidth = '';
      renderCount++;
      const id = 'mmd-' + renderCount;
      const { svg } = await mermaid.render(id, code);
      container.innerHTML = svg;
      await new Promise(r => requestAnimationFrame(() => requestAnimationFrame(r)));
      const svgEl = container.querySelector('svg');
      if (svgEl) {
        const rect = svgEl.getBoundingClientRect();
        return JSON.stringify({ width: Math.ceil(rect.width), height: Math.ceil(rect.height), ok: true });
      }
      return JSON.stringify({ ok: false, error: 'SVG not found' });
    } catch (e) {
      return JSON.stringify({ ok: false, error: e.message || String(e) });
    }
  }

  window.addEventListener('load', function() {
    window.chrome.webview.postMessage(mermaidReady() ? 'mermaid-ready' : 'mermaid-failed');
  });
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
    std::wstring path = std::wstring(appdata) + L"\\MaDView\\WebView2Data";
    CoTaskMemFree(appdata);
    std::filesystem::create_directories(path);
    return path;
}

// FNV-1a 64-bit hash
static std::wstring SimpleHash(const std::wstring& input) {
    uint64_t hash = 14695981039346656037ULL;
    for (wchar_t c : input) {
        hash ^= static_cast<uint64_t>(c);
        hash *= 1099511628211ULL;
    }
    wchar_t buf[20];
    swprintf_s(buf, L"%016llx", hash);
    return buf;
}

// Escape a wstring for embedding as a JavaScript string literal (single-quoted)
static std::wstring JsEscape(const std::wstring& input) {
    std::wstring result;
    result.reserve(input.size() + input.size() / 4);
    for (wchar_t c : input) {
        switch (c) {
            case L'\\': result += L"\\\\"; break;
            case L'\'': result += L"\\'"; break;
            case L'"':  result += L"\\\""; break;
            case L'\n': result += L"\\n"; break;
            case L'\r': result += L"\\r"; break;
            case L'\t': result += L"\\t"; break;
            case L'`':  result += L"\\`"; break;
            case L'$':  result += L"\\$"; break;
            default:
                if (c < 0x20) {
                    wchar_t buf[8];
                    swprintf_s(buf, L"\\u%04x", static_cast<unsigned>(c));
                    result += buf;
                } else {
                    result += c;
                }
                break;
        }
    }
    return result;
}

// Window class name for the offscreen WebView2 host
static const wchar_t* kMermaidHostClass = L"MaDView_MermaidHost";

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

    // Compute DPI scale for converting DIPs → physical pixels
    UINT dpi = GetDpiForWindow(hwnd);
    dpi_scale_ = dpi > 0 ? static_cast<float>(dpi) / 96.0f : 1.0f;

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
        OutputDebugStringW(L"[MaDView/Mermaid] Failed to create offscreen host window\n");
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
                    OutputDebugStringW(L"[MaDView/Mermaid] WebView2 environment creation failed\n");
                    return S_OK;
                }
                OutputDebugStringW(L"[MaDView/Mermaid] WebView2 environment created\n");

                webview_env_ = env;
                env->CreateCoreWebView2Controller(
                    webview_hwnd_,
                    Microsoft::WRL::Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
                        [this, on_ready](HRESULT result, ICoreWebView2Controller* controller) -> HRESULT {
                            if (FAILED(result) || !controller) {
                                OutputDebugStringW(L"[MaDView/Mermaid] WebView2 controller creation failed\n");
                                return S_OK;
                            }
                            OutputDebugStringW(L"[MaDView/Mermaid] WebView2 controller created\n");

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
                                            if (wcscmp(msg, L"mermaid-ready") == 0) {
                                                OutputDebugStringW(L"[MaDView/Mermaid] Received mermaid-ready\n");
                                                ready_ = true;
                                                if (on_ready) on_ready();
                                                ProcessQueue();
                                            } else if (wcsncmp(msg, L"render-result:", 14) == 0) {
                                                OnMermaidRenderResult(std::wstring(msg + 14));
                                            } else if (wcscmp(msg, L"capture-ready") == 0) {
                                                DoCapturePreview();
                                            } else if (wcsncmp(msg, L"render-error:", 13) == 0) {
                                                OutputDebugStringW(L"[MaDView/Mermaid] Render error via postMessage: ");
                                                OutputDebugStringW(msg + 13);
                                                OutputDebugStringW(L"\n");
                                                FinishCurrentRequest();
                                            } else {
                                                OutputDebugStringW(L"[MaDView/Mermaid] Received message: ");
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

                                        if (url.find(L"/mermaid.min.js") != std::wstring::npos) {
                                            // Serve mermaid.js from embedded resource
                                            std::string js = LoadMermaidJsFromResource();
                                            OutputDebugStringW(L"[MaDView/Mermaid] Serving mermaid.min.js from resource (");
                                            OutputDebugStringW(std::to_wstring(js.size()).c_str());
                                            OutputDebugStringW(L" bytes)\n");
                                            stream = CreateMemoryStream(js.data(), js.size());
                                            headers = L"Content-Type: application/javascript; charset=utf-8";
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

std::wstring MermaidRenderer::HashCode(const std::wstring& code, float max_width, bool dark_mode) const {
    std::wstring key = code + L"|" + std::to_wstring(static_cast<int>(max_width))
                       + L"|" + (dark_mode ? L"d" : L"l");
    return SimpleHash(key);
}

void MermaidRenderer::RequestRender(RenderNode& node, float max_width, bool dark_mode,
                                     std::function<void()> on_complete) {
    if (node.code_language != SyntaxLanguage::Mermaid) return;

    std::wstring hash = HashCode(node.text, max_width, dark_mode);

    // Check cache first
    auto it = cache_.find(hash);
    if (it != cache_.end()) {
        node.diagram_bitmap = it->second.bitmap;
        node.diagram_width = it->second.width;
        node.diagram_height = it->second.height;
        node.height = it->second.height;
        node.layout_dirty = false;
        if (on_complete) on_complete();
        return;
    }

    // Queue the request
    RenderRequest req;
    req.node = &node;
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

    // max_width is in DIPs.  WebView2 bounds are in physical pixels.
    int vp_px = static_cast<int>(max_width * dpi_scale_);
    if (vp_px < 400) vp_px = 400;
    RECT bounds = {0, 0, vp_px, static_cast<LONG>(4096 * dpi_scale_)};
    webview_controller_->put_Bounds(bounds);
    SetWindowPos(webview_hwnd_, nullptr, -32000, -32000, vp_px,
                 static_cast<int>(4096 * dpi_scale_),
                 SWP_NOZORDER | SWP_NOACTIVATE);

    // Call renderMermaid and deliver result via postMessage.
    std::wstring js = L"renderMermaid('" + JsEscape(code) + L"', "
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
        return num.empty() ? 0.0f : std::stof(num);
    };

    dw = find_num(L"\"width\"");
    dh = find_num(L"\"height\"");
    ok = json.find(L"\"ok\":true") != std::wstring::npos
         || json.find(L"\"ok\": true") != std::wstring::npos;

    if (!ok || dw <= 0 || dh <= 0) {
        OutputDebugStringW(L"[MaDView/Mermaid] renderMermaid returned error or zero size\n");
        OutputDebugStringW((L"[MaDView/Mermaid]   result: " + json + L"\n").c_str());
        FinishCurrentRequest();
        return;
    }
    {
        std::wstring msg = L"[MaDView/Mermaid] renderMermaid ok: "
            + std::to_wstring(static_cast<int>(dw)) + L"x"
            + std::to_wstring(static_cast<int>(dh)) + L"\n";
        OutputDebugStringW(msg.c_str());
    }

    // Store CSS pixel dimensions for later use as drawing size (DIPs)
    current_request_.css_width = dw;
    current_request_.css_height = dh;

    // Resize WebView to exact diagram size for capture (convert CSS px → physical px)
    int cw = static_cast<int>(std::ceil(dw * dpi_scale_));
    int ch = static_cast<int>(std::ceil(dh * dpi_scale_));
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

    webview_->CapturePreview(
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
            std::wstring msg = L"[MaDView/Mermaid] Capture: bitmap="
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

        // Update the node
        if (current_request_.node) {
            current_request_.node->diagram_bitmap = bitmap;
            current_request_.node->diagram_width = draw_w;
            current_request_.node->diagram_height = draw_h;
            current_request_.node->height = draw_h;
            current_request_.node->layout_dirty = false;
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
