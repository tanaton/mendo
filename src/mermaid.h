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


class MermaidFileCache;

// オフスクリーンWebView2を使ってMermaidダイアグラムコードをID2D1Bitmapにレンダリングする。
// 複数のWebView2インスタンスを並行稼働させ、複数の図を同時にレンダリングできる。
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

    // コールバック型: ヒープ割り当てを回避するために関数ポインタ+コンテキストを使用
    using Callback = void(*)(void*);

    // Mermaidコードブロックのレンダリングを要求する。
    // 完了時、ダイアグラムエントリのbitmap/width/heightとレイアウトエントリの
    // height/layout_dirtyが設定され、on_completeがUIスレッドで呼び出される。
    void RequestRender(Node& node, NodeLayoutEntry& layout_entry, DiagramEntry& diagram_entry,
        float max_width, bool dark_mode,
        Callback on_complete, void* user_data);

    // D2Dレンダーターゲットを更新する（例：リサイズ後）。
    void SetRenderTarget(ID2D1RenderTarget* render_target);

    // ファイルキャッシュを設定する。Init()の前に呼び出す。
    void SetFileCache(MermaidFileCache* cache) noexcept { file_cache_ = cache; }

    // キャッシュされたビットマップをすべてクリアする。
    void ClearCache();

    // 保留中のリクエストをすべてキャンセルし、処理中のリクエストを無効化する。
    // nodesベクターが置き換えられる前に呼び出す必要がある。
    void CancelPending();

    // 保留キューのみをクリアする（処理中のレンダリングには影響しない）。
    // リサイズ時に古い幅のリクエストを破棄するために使用する。
    void ClearPendingQueue() noexcept;

private:
    struct RenderRequest {
        Node* node = nullptr;
        NodeLayoutEntry* layout_entry = nullptr;
        DiagramEntry* diagram_entry = nullptr;
        float max_width = 0.0f;
        bool dark_mode = false;
        Callback on_complete = nullptr;
        void* on_complete_data = nullptr;
        uint64_t code_hash = 0;
        float css_width = 0.0f;   // JSから取得したCSSピクセル寸法（DIP）
        float css_height = 0.0f;
        float dpr = 1.0f;         // JSから取得したdevicePixelRatio
        unsigned int request_id = 0; // リクエスト固有のID（JS側のpostMessageと照合）
    };

    static constexpr int kMaxWorkers = 4;

    // WebView2ワーカー: 各ワーカーが独立したWebView2インスタンスを持ち、
    // 1つのダイアグラムを非同期レンダリングできる。
    struct Worker {
        HWND hwnd = nullptr;
        Microsoft::WRL::ComPtr<ICoreWebView2Controller> controller;
        Microsoft::WRL::ComPtr<ICoreWebView2> webview;
        RenderRequest current_request;
        float dpr = 1.0f;
        bool rendering = false;
        bool ready = false;
    };

    static int ComputeWorkerCount() noexcept;
    void SetupWorker(int index);
    void ProcessQueue();
    void RenderInWorker(Worker& worker);
    void OnRenderResult(int worker_idx, std::wstring_view json);
    void DoCapturePreview(int worker_idx);
    void OnCaptureComplete(int worker_idx, uint64_t code_hash, IStream* png_stream);
    void FinishWorkerRequest(Worker& worker);
    uint64_t HashCode(std::wstring_view code, float max_width, bool dark_mode) const;
    HRESULT CreateBitmapFromPngStream(IStream* stream, ID2D1Bitmap** bitmap,
        float* width, float* height);

    HWND hwnd_ = nullptr;           // メインウィンドウ
    ID2D1RenderTarget* render_target_ = nullptr;
    Microsoft::WRL::ComPtr<IWICImagingFactory> wic_factory_;
    Microsoft::WRL::ComPtr<ICoreWebView2Environment> webview_env_;
    std::span<const std::byte> cached_mermaid_gz_; // Win32リソースから直接参照するgzip圧縮済みmermaid.js

    Worker workers_[kMaxWorkers];
    int worker_count_ = 0;
    bool ready_ = false;
    unsigned int request_counter_ = 0;
    std::function<void()> on_all_ready_; // 最初のワーカー準備完了時に1回だけ呼び出す

    std::queue<RenderRequest, std::pmr::deque<RenderRequest>> pending_requests_;

    // キャッシュ: code_hash -> {bitmap, width, height}
    struct CachedBitmap {
        Microsoft::WRL::ComPtr<ID2D1Bitmap> bitmap;
        float width = 0.0f;
        float height = 0.0f;
    };
    std::pmr::unordered_map<uint64_t, CachedBitmap> cache_;

    MermaidFileCache* file_cache_ = nullptr;
};
