#pragma once
#include "types.h"
#include "layout_cache.h"
#include "mermaid_util.h"
#include <d2d1.h>
#include <wincodec.h>
#include <wrl/client.h>
#include <wrl/event.h>
#include <WebView2.h>
#include <string>
#include <vector>
#include <functional>
#include <unordered_map>
#include <queue>

using Microsoft::WRL::ComPtr;

// Renders Mermaid diagram code to ID2D1Bitmap via an offscreen WebView2.
// All public methods must be called from the UI thread.
class MermaidRenderer {
public:
    MermaidRenderer() = default;
    ~MermaidRenderer();

    MermaidRenderer(const MermaidRenderer&) = delete;
    MermaidRenderer& operator=(const MermaidRenderer&) = delete;

    // Initialize the renderer. hwnd is the main app window.
    // render_target is used for creating D2D bitmaps.
    // on_ready is called (on UI thread) when WebView2 is initialized.
    void Init(HWND hwnd, ID2D1RenderTarget* render_target,
              std::function<void()> on_ready);

    // Returns true if WebView2 is initialized and ready to render.
    bool IsReady() const { return ready_; }

    // Request rendering of a mermaid code block.
    // When done, the diagram entry's bitmap/width/height and the layout entry's
    // height/layout_dirty will be set, and on_complete will be called on the UI thread.
    void RequestRender(Node& node, NodeLayoutEntry& layout_entry, DiagramEntry& diagram_entry,
                       float max_width, bool dark_mode,
                       std::function<void()> on_complete);

    // Update the D2D render target (e.g. after resize).
    void SetRenderTarget(ID2D1RenderTarget* render_target);

    // Clear all cached bitmaps.
    void ClearCache();

    // Cancel all pending requests and invalidate the in-flight request.
    // Must be called before the nodes vector is replaced.
    void CancelPending();

private:
    void ProcessQueue();
    void RenderMermaidInWebView(const std::wstring& code, float max_width, bool dark_mode);
    void OnMermaidRenderResult(const std::wstring& json);
    void DoCapturePreview();
    void OnCaptureComplete(const std::wstring& code_hash, IStream* png_stream);
    std::wstring HashCode(const std::wstring& code, float max_width, bool dark_mode) const;
    HRESULT CreateBitmapFromPngStream(IStream* stream, ID2D1Bitmap** bitmap,
                                      float* width, float* height);
    void FinishCurrentRequest();

    HWND hwnd_ = nullptr;           // main window
    HWND webview_hwnd_ = nullptr;   // dedicated offscreen popup for WebView2
    ID2D1RenderTarget* render_target_ = nullptr;
    float dpr_ = 1.0f;             // devicePixelRatio reported by WebView2 JS
    ComPtr<IWICImagingFactory> wic_factory_;
    ComPtr<ICoreWebView2Environment> webview_env_;
    ComPtr<ICoreWebView2Controller> webview_controller_;
    ComPtr<ICoreWebView2> webview_;
    bool ready_ = false;
    bool rendering_ = false;
    int render_counter_ = 0;

    struct RenderRequest {
        Node* node = nullptr;
        NodeLayoutEntry* layout_entry = nullptr;
        DiagramEntry* diagram_entry = nullptr;
        float max_width = 0.0f;
        bool dark_mode = false;
        std::function<void()> on_complete;
        std::wstring code_hash;
        float css_width = 0.0f;   // CSS pixel dimensions (DIPs) from JS
        float css_height = 0.0f;
        float dpr = 1.0f;         // devicePixelRatio from JS
    };
    std::queue<RenderRequest> pending_requests_;
    RenderRequest current_request_;

    // Cache: code_hash -> {bitmap, width, height}
    struct CachedBitmap {
        ComPtr<ID2D1Bitmap> bitmap;
        float width = 0.0f;
        float height = 0.0f;
    };
    std::unordered_map<std::wstring, CachedBitmap> cache_;
};
