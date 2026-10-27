#pragma once
#include "document_types.h"
#include "layout_cache.h"
#include "lru_cache.h"
#include "mermaid_renderer_interface.h"
#include "mermaid_util.h"
#include "utility.h"
#include "worker_latch.h"
#include <d2d1.h>
#include <wincodec.h>
#include <wrl/client.h>
#include <WebView2.h>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory_resource>
#include <queue>
#include <span>
#include <string>
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

    // wic は D2DRenderBackend と共有する PNG デコード用。
    // bg_scheduler はディスクキャッシュの読み込みと PNG デコードを UI スレッド外で行う先で、
    // 読み込み完了は app_msg::MERMAID_DISK_LOADED で hwnd に通知され、受信側が ProcessDiskLoads を呼ぶ。
    // user_data_folder は WebView2 のセッションデータの保存先（空なら WebView2 既定の場所）。
    // on_update は準備完了と結果が出たときに呼ぶ。準備前の要求と、結果を待つ図を要求し直させる。
    void Init(HWND hwnd, IWICImagingFactory* wic, MermaidFileCache& file_cache,
              TaskScheduler& bg_scheduler,
              const std::filesystem::path& user_data_folder, std::move_only_function<void()> on_update);

    bool RequestRender(const Node& node, DiagramEntry& diagram_entry, float max_width, bool dark_mode) override;
    void RequestSvg(std::string_view code, float max_width, bool dark_mode, SvgCallback callback) override;
    void SetRenderTarget(ID2D1RenderTarget* render_target);
    // ワーカーは画面外に置くため、PNG は本体ウィンドウではなくその位置のモニタの DPR で描かれる。
    // ワーカーを起動する前にディスクキャッシュを引くときの DPR に使う。
    static float EstimateWorkerDpr() noexcept;
    // deadline を過ぎた分は残し、HasLeftoverDiskLoads が真になる。
    void ProcessDiskLoads(std::chrono::steady_clock::time_point deadline = std::chrono::steady_clock::now() + kUploadSliceBudget);
    // 新しい結果は投函で知らせるので、続きが要るのは時間切れで残したときだけ。UI スレッド専用。
    bool HasLeftoverDiskLoads() const noexcept
    {
        return disk_results_.HasLeftover();
    }
    void Shutdown();
    void DropQueued() override;
    void OnInitRetryTimer();
    // 一定時間描画要求が無ければ WebView2 を環境ごと閉じる (browser/renderer/GPU プロセスのメモリ解放)。
    void OnIdleTimer();
    // 図のある文書を書き換えながら見ている (変更の通知で読み直した) 間は、保存のたびにコールドスタートしないよう長めに残す。
    void SetEditingDiagrams(bool editing);

private:
    struct RenderRequest {
        // UTF-8 の原文。呼び出し側の文字列の寿命から切り離して持つ。WebView2 向けの変換は描く直前に行い、
        // 待ちの間 (準備前・ディスク読み込み中・キュー待ち) は原文の大きさのままにする。
        std::pmr::string code;
        // LaTeX 数式。flowchart で包んで描く。
        bool latex = false;
        float max_width = 0.0f;
        bool dark_mode = false;
        // 幅つきのキー。要求中の判定とエラー結果はこちらで持つ。
        uint64_t code_hash = 0;
        uint64_t width_free_hash = 0;
        // 結果を width_free_hash で持つ。ディスクで当たったか、描いた結果が幅の上限より狭かった。
        bool width_free = false;
        uint64_t CacheKey() const noexcept
        {
            return width_free ? width_free_hash : code_hash;
        }
        float css_width = 0.0f; // JSから取得したCSSピクセル寸法（DIP）
        float css_height = 0.0f;
        unsigned int request_id = 0; // リクエスト固有のID（JS側のpostMessageと照合）
        // プロセス障害からの requeue 済みフラグ。クラッシュ誘発入力での再起動ループを防ぐ。
        bool retried = false;

        // SVGクリップボードコピー用リクエスト。true の場合キャッシュには入れず、
        // SVG文字列を svg_callback で返す。
        bool svg_only = false;
        SvgCallback svg_callback;
    };

    // 各ワーカーが独立したWebView2インスタンスを持ち、1つのダイアグラムを非同期レンダリングする。
    struct Worker {
        HWND hwnd = nullptr;
        Microsoft::WRL::ComPtr<ICoreWebView2Controller> controller;
        Microsoft::WRL::ComPtr<ICoreWebView2> webview;
        RenderRequest current_request;
        float dpr = 1.0f;
        int init_retries = 0;
        bool ready = false;

        // request_id は ProcessQueue で 1 以上を振り、完了・破棄で current_request ごと 0 に戻す。
        bool IsRendering() const noexcept
        {
            return current_request.request_id != 0;
        }
        bool IsIdle() const noexcept
        {
            return ready && !IsRendering();
        }
    };

    // 描画結果。同じコードは同じ結果になるので、失敗も成功と同じく覚えて描き直さない。
    struct CachedResult {
        Microsoft::WRL::ComPtr<ID2D1Bitmap> bitmap;
        float width = 0.0f;
        float height = 0.0f;
        // クリップボードコピー用 PNG。bitmap と同じ寿命で DiagramEntry へ伝播する。
        PngBytes png;
        // 非空なら失敗で、bitmap は持たない。
        DiagramError error;
    };

    struct DiskLoad {
        RenderRequest req;
        // worker でデコード確定済み。失敗時は null で read_error に理由が入る。
        Microsoft::WRL::ComPtr<IWICBitmap> bitmap;
        PngBytes png;
        // WebView2 の CapturePreview 出力。worker で png へ読み出したら解放する。
        Microsoft::WRL::ComPtr<IStream> captured_stream;
        // WebView2 で描画した直後の PNG。成功時はディスクキャッシュへ書き出す。
        bool captured = false;
        DWORD read_error = 0;
        // 投入時の disk_drop_gen_。worker で変わっていたら読まずに dropped で返す。
        uint32_t drop_gen = 0;
        bool dropped = false;
    };

    // 1 つ増やすごとにレンダラープロセスの起動と mermaid.js のロードが要り、数十 MB 常駐する。
    // 2 回目以降はディスクキャッシュで済むので、並列度は控えめにする。
    static constexpr int MAX_WORKERS = 2;
    static constexpr int MAX_ENV_RETRIES = 3;
    static constexpr UINT ENV_RETRY_INTERVAL_MS = 500;
    static constexpr int MAX_WORKER_RETRIES = 3;
    // 閉じると次の図で環境生成からやり直すため、読み進める間に閉じない程度に長めにする。
    static constexpr UINT IDLE_SHUTDOWN_MS = 60000;
    // 編集中は考えている間に閉じると保存のたびに 1 秒前後待たせるので、さらに長くする。
    static constexpr UINT IDLE_SHUTDOWN_EDITING_MS = 10 * 60000;
    // エラーも含む。書きかけの図を保存するたびにエラーが増えても、古いものから捨てる。
    static constexpr size_t MAX_CACHE_ENTRIES = 128;
    // ディスクキャッシュから戻せるので画像より控えめにする。
    static constexpr size_t MAX_CACHE_BYTES = 64u * 1024 * 1024;

    // SVG 専用リクエストならコールバックを呼んで svg_callback をクリアする (cancel / render-error / svg-result 共通)。
    static void InvokeSvgCallbackIfAny(RenderRequest& req, std::pmr::wstring svg, bool cancelled);
    // 表示用リクエストのエラーを kind で確定させる (issue #271)。msg は Render のときの Mermaid の文言で、空なら汎用の文言を出す。
    void SetDiagramError(const RenderRequest& req, DiagramError::Kind kind, std::wstring_view msg = {});
    static void ApplyResult(DiagramEntry& diagram_entry, const CachedResult& result);
    // 進行中の要求をすべて捨てる。SVG 要求は cancelled=true で完了させる。
    void CancelPending();

    std::span<Worker> ActiveWorkers() noexcept
    {
        return { workers_, static_cast<size_t>(worker_count_) };
    }
    Worker* FindIdleWorker() noexcept;

    void EnsureInitialized();
    void CreateWebView2Environment();
    void OnEnvironmentCreated(HRESULT result, ICoreWebView2Environment* env);
    bool CreateWorkerWindow(int index);
    // 全ワーカーが準備完了かつ描画中なのに待ちがあるときだけ 1 つ増やす。最初から最大数を
    // 同時起動すると CPU を取り合って最初の図が遅れ、図が少ない文書でも renderer が常駐する。
    void MaybeGrowWorkers();
    void ScheduleIdleShutdown();
    void ReleaseWebView();
    void SetupWorker(int index);
    void OnControllerCreated(int index, ICoreWebView2Controller* controller);
    void RegisterWebViewHandlers(int index);
    // 仮想ホスト app.local への要求に埋め込みリソースを返し、それ以外は遮断する。
    void OnWebResourceRequested(ICoreWebView2WebResourceRequestedEventArgs* args);
    // WebView2 プロセス障害 (ProcessFailed) からワーカーを復旧する。
    void RecoverWorker(int index);
    void DestroyWorker(Worker& w);
    static void ResizeWorkerView(Worker& worker, int width, int height);

    void ProcessQueue();
    void DrainPendingRequests(bool cancelled);
    void RenderInWorker(Worker& worker);
    // WebView2 ワーカーでの描画待ちキューに積む (初期化前なら初期化だけ起動して戻る)。
    void EnqueueWebRender(RenderRequest req);
    // 要求の結果 (ビットマップかエラー) がキャッシュに入っていれば、通知して要求し直させる。
    void FinishWorkerRequest(Worker& worker);
    // 要求を inflight_ に残したまま、ワーカーを次の要求に回す。
    void FreeWorker(Worker& worker);
    void ReleaseInflight(const RenderRequest& req) noexcept
    {
        if (!req.svg_only) {
            inflight_.erase(req.code_hash);
        }
    }
    void NotifyUpdate()
    {
        if (on_update_) {
            on_update_();
        }
    }

    // captured_stream があればファイルは読まずにそれをデコードする。
    void StartDiskLoad(RenderRequest req, std::filesystem::path png_path, Microsoft::WRL::ComPtr<IStream> captured_stream = nullptr);
    // worker で呼ぶ。
    void LoadDiskJob(DiskLoad& job, const std::filesystem::path& path);
    // 戻り値: 結果 (ビットマップかエラー) が出たか。
    bool ApplyDiskLoad(DiskLoad& r);

    void DispatchWebMessage(int index, const mermaid_util::ParsedWebMessage& parsed);
    void OnWorkerReady(Worker& worker, float dpr);
    void OnRenderResult(int worker_idx, const mermaid_util::RenderSize& size);
    void DoCapturePreview(int worker_idx);
    void OnCaptureComplete(int worker_idx, Microsoft::WRL::ComPtr<IStream> png_stream);
    void InsertCache(uint64_t hash, CachedResult result);
    const CachedResult* FindCached(uint64_t code_hash, uint64_t width_free_hash, float max_width);
    const CachedResult* FindCached(const RenderRequest& req)
    {
        return FindCached(req.code_hash, req.width_free_hash, req.max_width);
    }

    HWND hwnd_ = nullptr;
    ID2D1RenderTarget* render_target_ = nullptr;
    Microsoft::WRL::ComPtr<IWICImagingFactory> wic_factory_;
    Microsoft::WRL::ComPtr<ICoreWebView2Environment> webview_env_;
    std::pmr::wstring user_data_folder_;
    int env_retry_count_ = 0;

    Worker workers_[MAX_WORKERS];
    int worker_count_ = 0;
    // 最初のワーカーの準備ができたか。アイドル解放で戻す。
    bool ready_ = false;
    unsigned int request_counter_ = 0;
    // 準備完了 (アイドル解放後の再初期化を含む) と結果が出たときに呼ぶ。準備前の描画要求は捨てているため再要求させる。
    std::move_only_function<void()> on_update_;

    std::queue<RenderRequest, std::pmr::deque<RenderRequest>> pending_requests_;
    // 待機中・読み込み中・描画中の表示用リクエストのコードハッシュ。未完了の図はスクロールのたびに
    // 再要求されるため、ここで重複を弾く (二重描画・キュー肥大の防止)。
    std::pmr::unordered_set<uint64_t> inflight_;

    // code_hash -> CachedResult (LruCache の挙動は src/util/lru_cache.h 参照)。
    LruCache<uint64_t, CachedResult, MAX_CACHE_ENTRIES> cache_;
    MermaidFileCache* file_cache_ = nullptr;

    TaskScheduler* bg_scheduler_ = nullptr;
    bool editing_diagrams_ = false;
    PostedInbox<DiskLoad> disk_results_;
    // DropQueued / CancelPending で進め、投入済みのディスクキャッシュの読み込みを取り消す (ImageLoader の drop_gen_ と同じ)。
    std::atomic<uint32_t> disk_drop_gen_{ 0 };

    // Shutdown で worker 完了を待つ。scheduler 共有 worker から self を参照する race を排除する。
    WorkerLatch latch_;
};
