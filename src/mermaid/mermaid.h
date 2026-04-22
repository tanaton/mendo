#pragma once
#include "document_types.h"
#include "layout_cache.h"
#include "mermaid_renderer_interface.h"
#include "mermaid_util.h"
#include <d2d1.h>
#include <wincodec.h>
#include <wrl/client.h>
#include <wrl/event.h>
#include <WebView2.h>
#include <string>
#include <vector>
#include <functional>
#include "lru_cache.h"
#include <queue>
#include <memory_resource>


class MermaidFileCache;

// オフスクリーンWebView2を使ってMermaidダイアグラムコードをID2D1Bitmapにレンダリングする。
// 複数のWebView2インスタンスを並行稼働させ、複数の図を同時にレンダリングできる。
// すべてのパブリックメソッドはUIスレッドから呼び出す必要がある。
class MermaidRenderer : public IMermaidRenderer {
public:
    MermaidRenderer() = default;
    ~MermaidRenderer() override;

    MermaidRenderer(const MermaidRenderer&) = delete;
    MermaidRenderer& operator=(const MermaidRenderer&) = delete;

    void Init(HWND hwnd, ID2D1RenderTarget* render_target, IWICImagingFactory* wic, std::move_only_function<void()> on_ready);

    constexpr bool IsReady() const noexcept { return ready_; }

    void RequestRender(Node& node, NodeLayoutEntry& layout_entry, DiagramEntry& diagram_entry, float max_width, bool dark_mode, Callback on_complete) override;
    void SetRenderTarget(ID2D1RenderTarget* render_target);
    void SetFileCache(MermaidFileCache* cache) noexcept { file_cache_ = cache; }
    void ClearCache() override;
    void Shutdown();
    void CancelPending() override;
    void OnInitRetryTimer();
    static constexpr UINT_PTR TIMER_INIT_RETRY = 12;

#ifdef MENDO_TESTING
    constexpr bool IsInitialized() const noexcept { return initialized_; }
#endif

private:
    struct RenderRequest {
        Node* node = nullptr;
        NodeLayoutEntry* layout_entry = nullptr;
        DiagramEntry* diagram_entry = nullptr;
        float max_width = 0.0f;
        bool dark_mode = false;
        Callback on_complete;
        uint64_t code_hash = 0;
        float css_width = 0.0f;   // JSから取得したCSSピクセル寸法（DIP）
        float css_height = 0.0f;
        float dpr = 1.0f;         // JSから取得したdevicePixelRatio
        unsigned int request_id = 0; // リクエスト固有のID（JS側のpostMessageと照合）
    };

    static constexpr int MAX_WORKERS = 4;

    // WebView2ワーカー: 各ワーカーが独立したWebView2インスタンスを持ち、
    // 1つのダイアグラムを非同期レンダリングできる。
    struct Worker {
        HWND hwnd = nullptr;
        Microsoft::WRL::ComPtr<ICoreWebView2Controller> controller;
        Microsoft::WRL::ComPtr<ICoreWebView2> webview;
        RenderRequest current_request;
        float dpr = 1.0f;
        int init_retries = 0;
        bool rendering = false;
        bool ready = false;
    };

    static int ComputeWorkerCount() noexcept;
    void EnsureInitialized();
    void CreateWebView2Environment();
    void SetupWorker(int index);
    void ProcessQueue();
    void RenderInWorker(Worker& worker);
    void OnRenderResult(int worker_idx, std::wstring_view json);
    void DoCapturePreview(int worker_idx);
    void OnCaptureComplete(int worker_idx, uint64_t code_hash, IStream* png_stream);
    void FinishWorkerRequest(Worker& worker);
    uint64_t HashCode(std::string_view code_utf8, float max_width, bool dark_mode) const noexcept;
    HRESULT CreateBitmapFromPngStream(IStream* stream, ID2D1Bitmap** bitmap,
        float* width, float* height);

    HWND hwnd_ = nullptr;           // メインウィンドウ
    ID2D1RenderTarget* render_target_ = nullptr;
    Microsoft::WRL::ComPtr<IWICImagingFactory> wic_factory_;
    Microsoft::WRL::ComPtr<ICoreWebView2Environment> webview_env_;

    Worker workers_[MAX_WORKERS];
    int worker_count_ = 0;
    bool initialized_ = false;
    bool ready_ = false;
    unsigned int request_counter_ = 0;
    std::move_only_function<void()> on_all_ready_; // 最初のワーカー準備完了時に1回だけ呼び出す

    std::queue<RenderRequest, std::pmr::deque<RenderRequest>> pending_requests_;

    // キャッシュ: code_hash -> {bitmap, width, height}（LRU、最大64エントリ）
    struct CachedBitmap {
        Microsoft::WRL::ComPtr<ID2D1Bitmap> bitmap;
        float width = 0.0f;
        float height = 0.0f;
    };
    static constexpr size_t MAX_CACHE_ENTRIES = 64;
    LruCache<uint64_t, CachedBitmap> cache_{ MAX_CACHE_ENTRIES };

    MermaidFileCache* file_cache_ = nullptr;

    // WebView2環境生成リトライ
    static constexpr int MAX_ENV_RETRIES = 3;
    static constexpr int MAX_WORKER_RETRIES = 3;
    int env_retry_count_ = 0;
};
