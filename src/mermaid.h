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
#include <span>
#include <memory_resource>

using Microsoft::WRL::ComPtr;

// オフスクリーンWebView2を使ってMermaidダイアグラムコードをID2D1Bitmapにレンダリングする。
// すべてのパブリックメソッドはUIスレッドから呼び出す必要がある。
class MermaidRenderer {
public:
    MermaidRenderer() = default;
    ~MermaidRenderer();

    MermaidRenderer(const MermaidRenderer&) = delete;
    MermaidRenderer& operator=(const MermaidRenderer&) = delete;

    // レンダラーを初期化する。hwndはメインアプリウィンドウ。
    // render_targetはD2Dビットマップの作成に使用する。
    // on_readyはWebView2の初期化完了時にUIスレッドで呼び出される。
    void Init(HWND hwnd, ID2D1RenderTarget* render_target,
              std::function<void()> on_ready);

    // WebView2が初期化済みでレンダリング可能な場合にtrueを返す。
    constexpr bool IsReady() const noexcept { return ready_; }

    // Mermaidコードブロックのレンダリングを要求する。
    // 完了時、ダイアグラムエントリのbitmap/width/heightとレイアウトエントリの
    // height/layout_dirtyが設定され、on_completeがUIスレッドで呼び出される。
    void RequestRender(Node& node, NodeLayoutEntry& layout_entry, DiagramEntry& diagram_entry,
                       float max_width, bool dark_mode,
                       std::function<void()> on_complete);

    // D2Dレンダーターゲットを更新する（例：リサイズ後）。
    void SetRenderTarget(ID2D1RenderTarget* render_target);

    // キャッシュされたビットマップをすべてクリアする。
    void ClearCache();

    // 保留中のリクエストをすべてキャンセルし、処理中のリクエストを無効化する。
    // nodesベクターが置き換えられる前に呼び出す必要がある。
    void CancelPending();

private:
    void ProcessQueue();
    void RenderMermaidInWebView(std::wstring_view code, float max_width, bool dark_mode);
    void OnMermaidRenderResult(std::wstring_view json);
    void DoCapturePreview();
    void OnCaptureComplete(std::wstring_view code_hash, IStream* png_stream);
    std::pmr::wstring HashCode(std::wstring_view code, float max_width, bool dark_mode) const;
    HRESULT CreateBitmapFromPngStream(IStream* stream, ID2D1Bitmap** bitmap,
                                      float* width, float* height);
    void FinishCurrentRequest();

    HWND hwnd_ = nullptr;           // メインウィンドウ
    HWND webview_hwnd_ = nullptr;   // WebView2専用のオフスクリーンポップアップ
    ID2D1RenderTarget* render_target_ = nullptr;
    float dpr_ = 1.0f;             // WebView2 JSから報告されるdevicePixelRatio
    ComPtr<IWICImagingFactory> wic_factory_;
    ComPtr<ICoreWebView2Environment> webview_env_;
    ComPtr<ICoreWebView2Controller> webview_controller_;
    ComPtr<ICoreWebView2> webview_;
    bool ready_ = false;
    bool rendering_ = false;
    int render_counter_ = 0;
    std::span<const std::byte> cached_mermaid_gz_; // Win32リソースから直接参照するgzip圧縮済みmermaid.js

    struct RenderRequest {
        Node* node = nullptr;
        NodeLayoutEntry* layout_entry = nullptr;
        DiagramEntry* diagram_entry = nullptr;
        float max_width = 0.0f;
        bool dark_mode = false;
        std::function<void()> on_complete;
        std::pmr::wstring code_hash;
        float css_width = 0.0f;   // JSから取得したCSSピクセル寸法（DIP）
        float css_height = 0.0f;
        float dpr = 1.0f;         // JSから取得したdevicePixelRatio
    };
    std::queue<RenderRequest, std::pmr::deque<RenderRequest>> pending_requests_;
    RenderRequest current_request_;

    // キャッシュ: code_hash -> {bitmap, width, height}
    struct CachedBitmap {
        ComPtr<ID2D1Bitmap> bitmap;
        float width = 0.0f;
        float height = 0.0f;
    };
    std::pmr::unordered_map<std::pmr::wstring, CachedBitmap> cache_;
};
