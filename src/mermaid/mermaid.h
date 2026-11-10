#pragma once
#include "document_types.h"
#include "diagram_view.h"
#include "inline_math.h"
#include "layout_cache.h"
#include "transposition_cache.h"
#include "mermaid_renderer_interface.h"
#include "mermaid_util.h"
#include "pele_engine.h"
#include "utility.h"
#include "worker_latch.h"
#include <d2d1_3.h>
#include <dwrite.h>
#include <wincodec.h>
#include <wrl/client.h>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <deque>
#include <memory>
#include <optional>
#include <string>
#include <unordered_set>
#include <variant>

class MermaidFileCache;
class TaskScheduler;

// インライン数式をまとめて描いた結果。
struct InlineMathBatchResult {
    struct Item {
        uint64_t key = 0;
        // 描けなかった数式 (KaTeX のエラー) は空。
        InlineMathMetrics metrics;
        // 描けた数式の描画命令。原点は箱の左端のベースライン (metrics.baseline)。
        std::shared_ptr<const MathDrawing> drawing;
    };
    // 要求と同じ順。
    std::vector<Item> items;
};
// 描けたときと失敗したときに 1 回だけ呼ぶ。取り消した (終了・DropQueuedMath) 要求では呼ばない。
using MathBatchCallback = std::move_only_function<void(InlineMathBatchResult result)>;

// Mermaid の図を描画命令にして ID2D1CommandList に記録する。図は bg のワーカーで pele を QuickJS で動かして描き
// (PeleEngine)、拡大表示 (DiagramView) と保存・コピーの画像もその描画命令から作る。図のラベルの数式とインライン数式は
// 同じランタイムで KaTeX を動かして描く。描画命令にできない図は描かず、描けないことを知らせる。
// すべてのパブリックメソッドはUIスレッドから呼び出す必要がある。
class MermaidRenderer : public IMermaidRenderer {
public:
    MermaidRenderer() = default;
    ~MermaidRenderer() override;

    MermaidRenderer(const MermaidRenderer&) = delete;
    MermaidRenderer& operator=(const MermaidRenderer&) = delete;

    // 最初に図か数式を描くときに 1 回だけ、Init の dw で呼ぶ。本番は CreateBundledPele で、テストはビルドで作ったバイトコードから
    // 作る (テストの exe は埋め込みリソースを持たない)。
    using PeleFactory = std::unique_ptr<PeleEngine> (*)(IDWriteFactory* dw);
    static std::unique_ptr<PeleEngine> CreateBundledPele(IDWriteFactory* dw);
    // 拡大表示の窓を開く。開けなければ null。本番は DiagramView::Open で、テストは窓を作らない実装を返す。
    using ViewerFactory = std::unique_ptr<IDiagramViewer> (*)(HWND owner, ID2D1Factory* d2d, IDWriteFactory* dw, IWICImagingFactory* wic, bool dark,
                                                              std::move_only_function<void()> on_close);

    // d2d・wic・dw は Renderer と共有する。d2d は拡大表示の窓と図を描いた画像の描画先を、wic は図を描いた画像を、
    // dw は図の文字を作るのに使う。
    // bg_scheduler は描画とディスクキャッシュの読み込みを UI スレッド外で行う先で、
    // 完了は app_msg::MERMAID_JOB_DONE で hwnd に通知され、受信側が ProcessJobResults を呼ぶ。
    // on_update は結果が出たときと、拡大表示に出す図が変わったときに呼ぶ。受けた側は本文の図と拡大表示の図
    // (RequestViewerDrawing) を要求し直す。
    // on_viewer_failed は拡大表示を開けなかったときに、ウィンドウを閉じてから呼ぶ。
    void Init(HWND hwnd, ID2D1Factory* d2d, IWICImagingFactory* wic, IDWriteFactory* dw, MermaidFileCache& file_cache, TaskScheduler& bg_scheduler,
              PeleFactory make_pele, ViewerFactory open_viewer, std::move_only_function<void()> on_update,
              std::move_only_function<void()> on_viewer_failed);

    bool RequestRender(const Node& node, DiagramEntry& diagram_entry, float max_width, bool dark_mode) override;
    // インライン数式をまとめて bg のワーカーで描く (PeleEngine の KaTeX。1 個 平均 0.2ms ほど)。図と同じ同時に描く数の枠を使う。
    // 速いのでディスクキャッシュには置かない。
    void RequestMathBatch(std::vector<InlineMathItem> items, MathBatchCallback callback);
    void SetRenderTarget(ID2D1RenderTarget* render_target);
    // 本体の窓の DPR で今その場で描く (大きな図は画素数の上限まで縮める)。
    Microsoft::WRL::ComPtr<IWICBitmap> DiagramBitmap(const mermaid_util::DiagramDrawing& drawing, float max_width) override;
    // deadline を過ぎた分は残し、HasLeftoverJobResults が真になる。描画先が無い間は何もしないので、戻ったら呼ぶ。
    void ProcessJobResults(std::chrono::steady_clock::time_point deadline = std::chrono::steady_clock::now() + kUploadSliceBudget);
    // 新しい結果は投函で知らせるので、続きが要るのは時間切れで残したときだけ。UI スレッド専用。
    bool HasLeftoverJobResults() const noexcept
    {
        return render_target_ && job_results_.HasLeftover();
    }
    void Shutdown();
    void DropQueued() override;
    // 描画待ちのインライン数式の要求を取り消す。数式の寸法表を捨てたときに、もう使わない組を描かせない。
    void DropQueuedMath();
    // PeleEngine の使っていないランタイムを捨てる。PELE_TRIM のタイマーと、別のアプリへ切り替わったときに呼ぶ。
    void TrimIdleRuntimes();

    // 図を別ウィンドウ (1 枚だけ) に開く。開いていれば中身を差し替える。code (UTF-8) は内部でコピーする。
    // node は呼び出し側の図の目印で、窓と一緒に消える (ViewerNode)。
    void OpenViewer(std::string_view code, int node, bool dark_mode);
    void CloseViewer();
    // 拡大表示は本文と同じ結果 (cache_) を使う。本文が描いた図はその描画命令をそのまま出し、無ければ表示用の要求として
    // 描かせる (同じ図を本文用と 2 回描かない。本文と同じ折り返しになる)。
    void RequestViewerDrawing(float max_width, bool dark_mode) override;
    // 開いていれば、倍率と位置を保って新しいテーマの図に差し替える。
    void SetViewerDarkMode(bool dark_mode);
    // 開いている図のコード。閉じていれば nullopt (図を空にして保存すると、開いていてもコードは空になる)。
    std::optional<std::string_view> ViewerCode() const noexcept
    {
        return ViewerOpen() ? std::optional<std::string_view>{ viewer_.code } : std::nullopt;
    }
    // 開いている図の目印。閉じていれば -1。読み直しで図を見失ったら SetViewerNode(-1) にする。
    int ViewerNode() const noexcept
    {
        return ViewerOpen() ? viewer_.node : -1;
    }
    void SetViewerNode(int node) noexcept
    {
        viewer_.node = node;
    }
    // 保存で図が変わったら、位置とボタンで変えた倍率を保って差し替える。描けなければ今の図を残す。開いていない・同じコードなら何もしない。
    void UpdateViewerCode(std::string_view code);
    void ApplyViewerLanguage();

private:
    struct MathRequest {
        std::vector<InlineMathItem> items;
        MathBatchCallback callback;
    };

    // 図 (本文と拡大表示) のキャッシュのキー。
    struct DisplayTarget {
        // 幅つきのキー。要求中の判定とエラー結果はこちらで持つ。
        uint64_t request_hash = 0;
        uint64_t width_free_hash = 0;
        // 結果を width_free_hash で持つ。ディスクで当たったか、描いた結果が幅の上限より狭かった。
        bool width_free = false;
        uint64_t CacheKey() const noexcept
        {
            return width_free ? width_free_hash : request_hash;
        }
    };

    struct RenderRequest {
        // UTF-8 の原文。呼び出し側の文字列の寿命から切り離して持つ。
        std::string code;
        float max_width = 0.0f;
        bool dark_mode = false;
        // 結果の渡し先。図 (DisplayTarget) は結果をキャッシュに持ち、数式 (MathRequest) はコールバックで返す。数式は code などを使わない。
        std::variant<DisplayTarget, MathRequest> target;
    };

    // 図の拡大表示 (窓は DiagramView が持つ)。
    struct Viewer {
        std::unique_ptr<IDiagramViewer> view;
        // 最後に要求した図。描けなかったコードも入るので、出ている絵のコードとは限らない。
        std::string code;
        int node = -1;
        // code の図の結果をまだ窓に渡していない。
        bool pending = false;
    };

    enum class JobWork : uint8_t {
        // PeleEngine で描く。同時に描く数の枠を 1 つ使い、結果を受け取ったときに返す (PumpRenders)。
        Render,
        // ディスクキャッシュの描画命令を読む。表示用の要求だけ。
        DiskLoad,
    };
    // 描かず・読まずに返した。投入後に drop_gen_ が進んだ表示用の要求か、PeleEngine が取り消した (終了)。
    struct JobDropped {};
    // 描いたか読んだ描画命令。UI で commands にする。
    struct JobDrawing {
        std::shared_ptr<const mermaid_util::DiagramDrawing> drawing;
        // 描いたときだけ、ディスクキャッシュに書く描画命令 (UTF-8)。
        std::string bytes;
    };
    // PeleEngine で描けなかった。描き直しても同じ。
    struct JobFailed {
        DiagramError error;
    };
    // ディスクキャッシュを読めなかったか (理由)、解析できなかった (0)。
    struct JobUnreadable {
        DWORD read_error = 0;
    };
    // MathRequest::items と同じ順で、描けなかった数式は null。
    struct JobMaths {
        std::vector<std::shared_ptr<const MathDrawing>> drawings;
    };
    using JobOutcome = std::variant<JobDropped, JobDrawing, JobFailed, JobUnreadable, JobMaths>;

    // bg のワーカーの仕事 1 つとその結果。
    struct Job {
        RenderRequest req;
        // 既定値を置かない (Render だと指定し忘れた仕事も描画の枠を返す)。
        JobWork work;
        JobOutcome outcome;
        // PeleEngine のランタイムを使った (使っていないランタイムを捨てさせる)。
        bool used_pele = false;
    };

    // ランタイムは 5ms ほどで作り直せる。読み進める間の描画の切れ目では捨てない長さにする。
    // 別のアプリへ切り替わったときは待たずに捨てる (App::OnActivateApp)。
    static constexpr UINT PELE_TRIM_MS = 10000;
    // エラーも含む。書きかけの図を保存するたびにエラーが増えても、古いものから捨てる。
    static constexpr size_t MAX_CACHE_ENTRIES = 128;
    // 数えるのは描画命令だけで、ID2D1CommandList (その約 5 倍) は数えないので、普通の図では件数の上限が先に効く。
    // 表示中の結果は DiagramEntry と CommandList を共有し、余分に持つのは画面外の結果だけ。図 27 個の文書で、幅を 10 段
    // 変えるたびに全体を読み進めても 128 件で commit 約 10MB (15%)。CommandList を数えて 8MB にしても -6% で、
    // テーマの切り替え (58 件、5%) には効かない。結果はここを経由してしか DiagramEntry に届かないので、
    // 1 回に届く結果より小さな上限にすると描き直しが回り続ける。
    static constexpr size_t MAX_CACHE_BYTES = 64u * 1024 * 1024;

    void ApplyMathResult(Job& r);
    bool ViewerOpen() const noexcept
    {
        return viewer_.view && viewer_.view->IsOpen();
    }
    // 拡大表示に出す図が変わった。待ちの印を付け、on_update で頼ませる (印だけだと窓が読み込み中のまま残る)。
    void AwaitViewerDrawing();
    void FailViewer();

    // 最初に図か数式を描くときに作る。bg のワーカーが使うので、UI スレッドで投げる前に呼ぶ。
    void EnsurePele();
    // bg のワーカーで PeleEngine に描かせ、結果をディスクキャッシュの読み込みと同じ経路で返す。
    void StartRender(RenderRequest req);
    // 同時に描く数の上限まで、待ちから bg へ出す。
    void PumpRenders();
    // req から Job を作って bg で work(job) し、その結果を outcome に入れて job_results_ に返す。投入後に drop_gen_ が進んだ
    // 表示用の要求は work せずに JobDropped で返す。
    template <typename Work>
    void PostJob(RenderRequest req, JobWork kind, Work work);
    // worker で呼ぶ。
    JobOutcome RenderJob(Job& job);

    void StartDiskLoad(RenderRequest req, std::filesystem::path path);
    // worker で呼ぶ。
    JobOutcome LoadDiskJob(Job& job, const std::filesystem::path& path);
    // 戻り値: 結果 (描画命令かエラー) が出たか。
    bool ApplyJobResult(Job& r);
    bool ApplyDisplayResult(Job& r);
    // 表示用の要求のエラーを確定させ、再要求させない (issue #271)。error.message は pele の文言で、空なら汎用の文言を出す。
    bool SettleDisplayError(const DisplayTarget& display, const DiagramError& error);

    // UI スレッドで呼ぶ。描画中の Renderer とは別の DeviceContext で記録する。記録できなければ null。
    Microsoft::WRL::ComPtr<ID2D1CommandList> RecordDrawing(const mermaid_util::DiagramDrawing& drawing);
    void InsertCache(uint64_t hash, DiagramResult result);
    // 幅を混ぜないキーの結果 (いまの幅の上限より狭いときだけ使える) を先に、無ければ幅つきのキーで引く。
    const DiagramResult* FindCached(uint64_t request_hash, uint64_t width_free_hash, float max_width);
    // 結果 (描画命令かエラー) が出ていれば返す。まだなら読み込みか描画を始めて null (始めていれば何もしない)。
    const DiagramResult* FindOrRequest(std::string_view code, float max_width, bool dark_mode);

    HWND hwnd_ = nullptr;
    ID2D1RenderTarget* render_target_ = nullptr;
    Microsoft::WRL::ComPtr<ID2D1Factory> d2d_factory_;
    Microsoft::WRL::ComPtr<IWICImagingFactory> wic_factory_;

    ViewerFactory open_viewer_ = nullptr;
    Viewer viewer_;
    std::move_only_function<void()> on_viewer_failed_;

    std::move_only_function<void()> on_update_;

    // 待機中・読み込み中・描画中の表示用リクエストのキー (request_hash。コード・量子化した幅・テーマ)。未完了の図はスクロールのたびに
    // 再要求されるため、ここで重複を弾く (二重描画・キュー肥大の防止)。
    std::unordered_set<uint64_t> inflight_;

    // キャッシュキー -> 描画結果 (挙動は src/util/transposition_cache.h 参照)。同じコードは同じ結果になるので、失敗も成功と
    // 同じく覚えて描き直さない。
    TranspositionCache<uint64_t, DiagramResult, MAX_CACHE_ENTRIES> cache_;
    MermaidFileCache* file_cache_ = nullptr;

    TaskScheduler* bg_scheduler_ = nullptr;
    PostedInbox<Job> job_results_;
    // DropQueued / Shutdown で進め、投入済みの表示用の図の描画とディスクキャッシュの読み込みを取り消す (ImageLoader の drop_gen_ と同じ)。
    std::atomic<uint32_t> drop_gen_{ 0 };

    // 最初に図か数式を描くときに作る (EnsurePele)。終了時は描き終わりを待ってから捨てる。
    PeleFactory make_pele_ = nullptr;
    std::unique_ptr<PeleEngine> pele_;
    // PeleEngine で描く要求のうち、上限を超えて待っているものと、bg で走っている数。
    std::deque<RenderRequest> render_waiting_;
    int renders_running_ = 0;

    // 描画命令を記録する DeviceContext。描画先と同じデバイスから作り、描画先が変わったら作り直す。
    Microsoft::WRL::ComPtr<ID2D1DeviceContext5> record_dc_;
    Microsoft::WRL::ComPtr<IDWriteFactory> dw_factory_;

    // Shutdown で worker 完了を待つ。scheduler 共有 worker から self を参照する race を排除する。
    WorkerLatch latch_;
};
