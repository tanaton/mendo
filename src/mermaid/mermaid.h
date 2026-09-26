#pragma once
#include "document_types.h"
#include "layout_cache.h"
#include "mermaid_renderer_interface.h"
#include "mermaid_util.h"
#include "wic_util.h"
#include <d2d1.h>
#include <wincodec.h>
#include <wrl/client.h>
#include <wrl/event.h>
#include <WebView2.h>
#include <string>
#include <vector>
#include <filesystem>
#include <functional>
#include "lru_cache.h"
#include "worker_latch.h"
#include <atomic>
#include <mutex>
#include <queue>
#include <memory>
#include <memory_resource>
#include <optional>
#include <unordered_set>


class MermaidFileCache;
class TaskScheduler;

// オフスクリーンWebView2を使ってMermaidダイアグラムコードをID2D1Bitmapにレンダリングする。
// 複数のWebView2インスタンスを並行稼働させ、複数の図を同時にレンダリングできる。
// すべてのパブリックメソッドはUIスレッドから呼び出す必要がある。
class MermaidRenderer : public IMermaidRenderer {
public:
    MermaidRenderer() = default;
    ~MermaidRenderer() override;

    MermaidRenderer(const MermaidRenderer&) = delete;
    MermaidRenderer& operator=(const MermaidRenderer&) = delete;

    // user_data_folder は WebView2 のセッションデータの保存先（空なら WebView2 既定の場所）。
    void Init(HWND hwnd, ID2D1RenderTarget* render_target, IWICImagingFactory* wic, const std::filesystem::path& user_data_folder, std::move_only_function<void()> on_ready);

    constexpr bool IsReady() const noexcept
    {
        return lifecycle_.IsReady();
    }

    void RequestRender(Node& node, NodeLayoutEntry& layout_entry, DiagramEntry& diagram_entry, float max_width, bool dark_mode, Callback on_complete) override;
    void RequestSvg(std::wstring_view code, float max_width, bool dark_mode, SvgCallback callback) override;
    void SetRenderTarget(ID2D1RenderTarget* render_target);
    void SetFileCache(MermaidFileCache* cache) noexcept
    {
        file_cache_ = cache;
    }
    // ディスクキャッシュの読み込み/PNG デコードと mermaid.js の展開を UI スレッド外で行う先。
    // 読み込み完了は disk_loaded_msg で hwnd に通知され、受信側が ProcessDiskLoads を呼ぶ。
    void SetBackgroundScheduler(TaskScheduler* scheduler, UINT disk_loaded_msg) noexcept
    {
        bg_scheduler_ = scheduler;
        disk_loaded_msg_ = disk_loaded_msg;
    }
    void ProcessDiskLoads();
    void ClearCache() override;
    void Shutdown();
    void CancelPending() override;
    void DropQueued() override;
    void OnInitRetryTimer();
    // 一定時間描画要求が無ければ先頭以外のワーカーを閉じる (renderer プロセスのメモリ解放)。
    void OnIdleTimer();

#ifdef MENDO_TESTING
    constexpr bool IsInitialized() const noexcept
    {
        return lifecycle_.IsInitialized();
    }
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
        float css_width = 0.0f; // JSから取得したCSSピクセル寸法（DIP）
        float css_height = 0.0f;
        unsigned int request_id = 0; // リクエスト固有のID（JS側のpostMessageと照合）
        // プロセス障害からの requeue 済みフラグ。クラッシュ誘発入力での再起動ループを防ぐ。
        bool retried = false;

        // SVGクリップボードコピー用リクエスト。true の場合 layout/diagram は使わず、
        // SVG文字列を svg_callback で返す。
        bool svg_only = false;
        SvgCallback svg_callback;
        // SVG リクエスト時のコード保持（呼び出し側の文字列ライフタイムから切り離す）
        std::pmr::wstring code_storage;
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

    // SVG 専用リクエストならコールバックを呼んで svg_callback をクリアする。
    // 同じパターン（cancel / render-error / svg-result の各経路）を1か所に集約する。
    static void InvokeSvgCallbackIfAny(RenderRequest& req, std::pmr::wstring svg, bool cancelled);

    // 表示用リクエストの diagram_entry にエラーを確定させる。msg が空なら i18n の
    // 汎用文言を入れ、「非空 = 失敗」の不変条件を保つ (issue #271)。
    static void SetDiagramError(RenderRequest& req, std::wstring_view msg);

    struct CachedBitmap;
    // キャッシュヒット時に layout_entry / diagram_entry の各フィールドを更新する。
    static void ApplyCachedBitmap(NodeLayoutEntry& layout_entry, DiagramEntry& diagram_entry, const CachedBitmap& cached) noexcept;
    void EnsureInitialized();
    void CreateWebView2Environment();
    bool CreateWorkerWindow(int index);
    // 全ワーカーが準備完了かつ描画中なのに待ちがあるときだけ 1 つ増やす。最初から最大数を
    // 同時起動すると CPU を取り合って最初の図が遅れ、図が少ない文書でも renderer が常駐する。
    void MaybeGrowWorkers();
    void ScheduleIdleShutdown();
    void SetupWorker(int index);
    // WebView2 プロセス障害 (ProcessFailed) からワーカーを復旧する。
    void RecoverWorker(int index);
    void ProcessQueue();
    void DrainPendingRequests(bool cancelled);
    void FailPendingRequests();
    void RenderInWorker(Worker& worker);
    // WebView2 ワーカーでの描画待ちキューに積む (初期化前なら初期化だけ起動して戻る)。
    void EnqueueWebRender(RenderRequest req);
    struct DiskLoad;
    // scheduler が無ければその場で読み込んで適用する。
    void StartDiskLoad(RenderRequest req, std::filesystem::path png_path);
    // worker で呼ぶ。
    void LoadDiskJob(DiskLoad& job, const std::filesystem::path& path);
    void ApplyDiskLoad(DiskLoad& r);
    void DestroyWorker(Worker& w);
    // 展開済み mermaid.js。未展開ならこの場で展開する。
    std::shared_ptr<const std::pmr::vector<uint8_t>> AcquireMermaidJs();
    void PrefetchMermaidJs();
    void OnRenderResult(int worker_idx, std::wstring_view json);
    // WebMessageReceived から受け取った parsed メッセージを worker[index] にディスパッチする。
    // ラムダ本体を 6 段ネストから 1 行に減らすため case 振り分けをメンバ関数に集約する。
    void DispatchWebMessage(int index, const mermaid_util::ParsedWebMessage& parsed);
    void DoCapturePreview(int worker_idx);
    void OnCaptureComplete(int worker_idx, IStream* png_stream);
    void FinishWorkerRequest(Worker& worker);
    std::optional<wic_util::CreatedBitmap> CreateBitmapFromPngStream(IStream* stream);

    HWND hwnd_ = nullptr; // メインウィンドウ
    ID2D1RenderTarget* render_target_ = nullptr;
    Microsoft::WRL::ComPtr<IWICImagingFactory> wic_factory_;
    Microsoft::WRL::ComPtr<ICoreWebView2Environment> webview_env_;
    std::pmr::wstring user_data_folder_;

    Worker workers_[MAX_WORKERS];
    int worker_count_ = 0;
    int target_worker_count_ = 0;
    mermaid_lifecycle::Lifecycle lifecycle_;
    unsigned int request_counter_ = 0;
    std::move_only_function<void()> on_all_ready_; // 最初のワーカー準備完了時に1回だけ呼び出す

    std::queue<RenderRequest, std::pmr::deque<RenderRequest>> pending_requests_;
    // 待機中/描画中の表示用リクエストの図。未完了の図はスクロールのたびに NeedsRender() が
    // true のまま再要求されるため、ここで重複を弾く (二重描画・キュー肥大の防止)。
    std::pmr::unordered_set<const DiagramEntry*> inflight_entries_;
    void ReleaseInflight(const RenderRequest& req) noexcept
    {
        if (req.diagram_entry) {
            inflight_entries_.erase(req.diagram_entry);
        }
    }

    // キャッシュ: code_hash -> {bitmap, width, height} (LruCache の挙動は src/util/lru_cache.h 参照)。
    struct CachedBitmap {
        Microsoft::WRL::ComPtr<ID2D1Bitmap> bitmap;
        float width = 0.0f;
        float height = 0.0f;
        // クリップボードコピー用 PNG。bitmap と同じ寿命で DiagramEntry へ伝播する。
        std::shared_ptr<const std::pmr::vector<uint8_t>> png;
    };
    static constexpr size_t MAX_CACHE_ENTRIES = 128;
    // ディスクキャッシュから戻せるので画像より控えめにする。
    static constexpr size_t MAX_CACHE_BYTES = 64u * 1024 * 1024;
    LruCache<uint64_t, CachedBitmap, MAX_CACHE_ENTRIES> cache_;
    void InsertCache(uint64_t hash, CachedBitmap cached);

    MermaidFileCache* file_cache_ = nullptr;

    struct DiskLoad {
        uint32_t gen = 0;
        RenderRequest req;
        // worker でデコード確定済み。失敗時は null で read_error に理由が入る。
        Microsoft::WRL::ComPtr<IWICBitmap> bitmap;
        std::shared_ptr<const std::pmr::vector<uint8_t>> png;
        DWORD read_error = 0;
    };
    TaskScheduler* bg_scheduler_ = nullptr;
    UINT disk_loaded_msg_ = 0;
    // CancelPending で進め、それ以前に投入した読み込み結果を捨てる。
    std::atomic<uint32_t> disk_gen_{ 0 };
    std::mutex disk_mutex_;
    std::pmr::vector<DiskLoad> disk_results_;

    std::mutex js_mutex_;
    std::shared_ptr<const std::pmr::vector<uint8_t>> js_bytes_;

    static constexpr UINT IDLE_SHUTDOWN_MS = 30000;

    // Shutdown で worker 完了を待つ。scheduler 共有 worker から self を参照する race を排除する。
    WorkerLatch latch_;

    // WebView2環境生成リトライ
    static constexpr int MAX_ENV_RETRIES = 3;
    static constexpr int MAX_WORKER_RETRIES = 3;
    int env_retry_count_ = 0;
};
