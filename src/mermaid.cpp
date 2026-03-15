#include "mermaid.h"
#include <shlwapi.h>
#include <shlobj.h>
#include <wrl/event.h>
#include <filesystem>
#include <functional>

#pragma comment(lib, "windowscodecs.lib")
#pragma comment(lib, "shlwapi.lib")

// Embedded HTML template that loads mermaid.js from CDN and renders diagrams.
// The page exposes renderMermaid(code, isDark) callable via ExecuteScript.
static const wchar_t* kMermaidHtmlTemplate = LR"HTML(
<!DOCTYPE html>
<html>
<head>
<meta charset="utf-8">
<style>
  html, body { margin: 0; padding: 0; overflow: hidden; background: transparent; }
  #container { display: inline-block; }
</style>
<script src="https://cdn.jsdelivr.net/npm/mermaid@11/dist/mermaid.min.js"></script>
<script>
  let initialized = false;
  async function renderMermaid(code, isDark) {
    try {
      if (!initialized) {
        mermaid.initialize({
          startOnLoad: false,
          theme: isDark ? 'dark' : 'default',
          securityLevel: 'strict'
        });
        initialized = true;
      }
      const container = document.getElementById('container');
      container.innerHTML = '';
      const { svg } = await mermaid.render('mermaid-diagram', code);
      container.innerHTML = svg;
      // Wait for rendering to complete
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
</script>
</head>
<body>
<div id="container"></div>
</body>
</html>
)HTML";

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

// Simple hash for cache key
static std::wstring SimpleHash(const std::wstring& input) {
    // FNV-1a 64-bit hash
    uint64_t hash = 14695981039346656037ULL;
    for (wchar_t c : input) {
        hash ^= static_cast<uint64_t>(c);
        hash *= 1099511628211ULL;
    }
    wchar_t buf[20];
    swprintf_s(buf, L"%016llx", hash);
    return buf;
}

// Escape a wstring for embedding as a JavaScript string literal
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

// ---- MermaidRenderer implementation ----

MermaidRenderer::~MermaidRenderer() {
    if (webview_controller_) {
        webview_controller_->Close();
    }
}

void MermaidRenderer::Init(HWND hwnd, ID2D1RenderTarget* render_target,
                           std::function<void()> on_ready) {
    hwnd_ = hwnd;
    render_target_ = render_target;

    // Create WIC factory for PNG decoding
    CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                     IID_PPV_ARGS(&wic_factory_));

    std::wstring user_data = GetWebView2UserDataFolder();

    // Create WebView2 environment
    CreateCoreWebView2EnvironmentWithOptions(
        nullptr, user_data.c_str(), nullptr,
        Microsoft::WRL::Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>(
            [this, on_ready](HRESULT result, ICoreWebView2Environment* env) -> HRESULT {
                if (FAILED(result) || !env) return S_OK;

                webview_env_ = env;
                env->CreateCoreWebView2Controller(
                    hwnd_,
                    Microsoft::WRL::Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
                        [this, on_ready](HRESULT result, ICoreWebView2Controller* controller) -> HRESULT {
                            if (FAILED(result) || !controller) return S_OK;

                            webview_controller_ = controller;
                            controller->get_CoreWebView2(&webview_);

                            // Hide the WebView (offscreen rendering)
                            controller->put_IsVisible(FALSE);
                            RECT bounds = {0, 0, 1, 1};
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

                            // Navigate to the mermaid HTML template
                            webview_->NavigateToString(kMermaidHtmlTemplate);

                            // Wait for navigation to complete
                            webview_->add_NavigationCompleted(
                                Microsoft::WRL::Callback<ICoreWebView2NavigationCompletedEventHandler>(
                                    [this, on_ready](ICoreWebView2* sender,
                                                     ICoreWebView2NavigationCompletedEventArgs* args) -> HRESULT {
                                        BOOL success = FALSE;
                                        args->get_IsSuccess(&success);
                                        if (success) {
                                            ready_ = true;
                                            if (on_ready) on_ready();
                                            ProcessQueue();
                                        }
                                        return S_OK;
                                    }).Get(),
                                nullptr);

                            return S_OK;
                        }).Get());
                return S_OK;
            }).Get());
}

void MermaidRenderer::SetRenderTarget(ID2D1RenderTarget* render_target) {
    render_target_ = render_target;
    // Cached bitmaps are tied to the old render target — clear them
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

void MermaidRenderer::RenderMermaidInWebView(const std::wstring& code, float max_width, bool dark_mode) {
    if (!webview_) return;

    // Set WebView size to accommodate the diagram
    int width = static_cast<int>(max_width);
    if (width < 200) width = 200;
    RECT bounds = {0, 0, width, 4096}; // tall enough for any diagram
    webview_controller_->put_Bounds(bounds);

    // Build JavaScript call
    std::wstring js = L"renderMermaid('" + JsEscape(code) + L"', "
                      + (dark_mode ? L"true" : L"false") + L")";

    webview_->ExecuteScript(js.c_str(),
        Microsoft::WRL::Callback<ICoreWebView2ExecuteScriptCompletedHandler>(
            [this](HRESULT hr, LPCWSTR resultJson) -> HRESULT {
                if (FAILED(hr) || !resultJson) {
                    // Rendering failed; skip this request
                    rendering_ = false;
                    if (current_request_.on_complete) current_request_.on_complete();
                    ProcessQueue();
                    return S_OK;
                }

                // Parse the result JSON: {"width":N,"height":N,"ok":true}
                std::wstring result(resultJson);

                // The result from ExecuteScript is JSON-encoded (wrapped in quotes)
                // Parse width/height from the JSON
                float dw = 0, dh = 0;
                bool ok = false;

                // Simple parse — result is a JSON string returned as "\"...\""
                // First, unwrap the outer quotes
                if (result.size() >= 2 && result.front() == L'"' && result.back() == L'"') {
                    // Unescape the inner JSON string
                    std::wstring inner;
                    inner.reserve(result.size());
                    for (size_t i = 1; i + 1 < result.size(); i++) {
                        if (result[i] == L'\\' && i + 2 < result.size()) {
                            i++;
                            switch (result[i]) {
                                case L'"': inner += L'"'; break;
                                case L'\\': inner += L'\\'; break;
                                case L'n': inner += L'\n'; break;
                                default: inner += result[i]; break;
                            }
                        } else {
                            inner += result[i];
                        }
                    }

                    // Parse width
                    auto find_num = [&](const std::wstring& key) -> float {
                        auto pos = inner.find(key);
                        if (pos == std::wstring::npos) return 0;
                        pos += key.size();
                        while (pos < inner.size() && (inner[pos] == L':' || inner[pos] == L' ')) pos++;
                        std::wstring num;
                        while (pos < inner.size() && (iswdigit(inner[pos]) || inner[pos] == L'.')) {
                            num += inner[pos++];
                        }
                        return num.empty() ? 0.0f : std::stof(num);
                    };

                    dw = find_num(L"\"width\"");
                    dh = find_num(L"\"height\"");
                    ok = inner.find(L"\"ok\":true") != std::wstring::npos
                         || inner.find(L"\"ok\": true") != std::wstring::npos;
                }

                if (!ok || dw <= 0 || dh <= 0) {
                    // Mermaid rendering failed — leave as text code block
                    rendering_ = false;
                    if (current_request_.on_complete) current_request_.on_complete();
                    ProcessQueue();
                    return S_OK;
                }

                // Now capture the WebView content as PNG
                // Resize to exact diagram size
                RECT capBounds = {0, 0, static_cast<LONG>(dw), static_cast<LONG>(dh)};
                webview_controller_->put_Bounds(capBounds);

                // Use CapturePreview to get the PNG
                IStream* pngStream = nullptr;
                CreateStreamOnHGlobal(nullptr, TRUE, &pngStream);

                webview_->CapturePreview(
                    COREWEBVIEW2_CAPTURE_PREVIEW_IMAGE_FORMAT_PNG,
                    pngStream,
                    Microsoft::WRL::Callback<ICoreWebView2CapturePreviewCompletedHandler>(
                        [this, pngStream](HRESULT hr) -> HRESULT {
                            if (SUCCEEDED(hr) && pngStream) {
                                OnCaptureComplete(current_request_.code_hash, pngStream);
                            } else {
                                rendering_ = false;
                                if (current_request_.on_complete) current_request_.on_complete();
                                ProcessQueue();
                            }
                            if (pngStream) pngStream->Release();
                            return S_OK;
                        }).Get());

                return S_OK;
            }).Get());
}

void MermaidRenderer::OnCaptureComplete(const std::wstring& code_hash, IStream* png_stream) {
    ComPtr<ID2D1Bitmap> bitmap;
    float bw = 0, bh = 0;

    if (SUCCEEDED(CreateBitmapFromPngStream(png_stream, &bitmap, &bw, &bh)) && bitmap) {
        // Store in cache
        CachedBitmap cached;
        cached.bitmap = bitmap;
        cached.width = bw;
        cached.height = bh;
        cache_[code_hash] = cached;

        // Update the node
        if (current_request_.node) {
            current_request_.node->diagram_bitmap = bitmap;
            current_request_.node->diagram_width = bw;
            current_request_.node->diagram_height = bh;
            current_request_.node->height = bh;
            current_request_.node->layout_dirty = false;
        }
    }

    rendering_ = false;
    if (current_request_.on_complete) current_request_.on_complete();
    ProcessQueue();
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
