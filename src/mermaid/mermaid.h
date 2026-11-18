#pragma once
#include "document_types.h"
#include "diagram_raster.h"
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
#include <unordered_map>
#include <unordered_set>
#include <variant>

class MermaidFileCache;
class TaskScheduler;

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

// Mermaid の図を描画命令にして ID2D1CommandList に記録する。図は bg のワーカーで pele を QuickJS で動かして描き (PeleEngine)、
// 拡大表示 (DiagramView) と保存・コピーの画像もその描画命令から作る。図のラベルの数式とインライン数式は同じランタイムの
// KaTeX で描く。描画命令にできない図は描かず、描けないことを知らせる。パブリックメソッドはすべて UI スレッドから呼ぶ。
class MermaidRenderer : public IMermaidRenderer {
public:
    // D3D の型をこのヘッダに持ち込まないよう、作る・壊すは mermaid.cpp で行う。
    MermaidRenderer();
    ~MermaidRenderer() override;

    MermaidRenderer(const MermaidRenderer&) = delete;
    MermaidRenderer& operator=(const MermaidRenderer&) = delete;

    // 最初に図か数式を描くときに 1 回だけ、Init の dw で呼ぶ。本番は CreateBundledPele で、テストはビルドで作ったバイトコードから
    // 作る (テストの exe は埋め込みリソースを持たない)。
    using PeleFactory = std::unique_ptr<PeleEngine> (*)(IDWriteFactory* dw);
    static std::unique_ptr<PeleEngine> CreateBundledPele(IDWriteFactory* dw);
    // 拡大表示の窓を開く。開けなければ null。本番は DiagramView::Open で、テストは窓を作らない実装を返す。
    // d3d と device は本体の描画デバイス (SetRenderTarget で受けたもの)。
    using ViewerFactory = std::unique_ptr<IDiagramViewer> (*)(HWND owner, ID3D11Device* d3d, ID2D1Device* device, IDWriteFactory* dw,
                                                              IWICImagingFactory* wic, bool dark, std::move_only_function<void()> on_close);

    // d2d・wic・dw は Renderer と共有し、保存・コピーの画像の描画先 (d2d) と画像 (wic)、図の文字 (dw) を作るのに使う。
    void Init(HWND hwnd, ID2D1Factory* d2d, IWICImagingFactory* wic, IDWriteFactory* dw, MermaidFileCache& file_cache, TaskScheduler& bg_scheduler,
              PeleFactory make_pele, ViewerFactory open_viewer, std::move_only_function<void()> on_update,
              std::move_only_function<void()> on_viewer_failed);

    bool RequestRender(const Node& node, DiagramEntry& diagram_entry, float max_width, bool dark_mode) override;
    // インライン数式をまとめて bg のワーカーで描く (PeleEngine の KaTeX)。図と同じ同時に描く数の枠を使う。
    // 速いのでディスクキャッシュには置かない。
    void RequestMathBatch(std::vector<InlineMathItem> items, MathBatchCallback callback);
    // d3d と device は render_target を作った描画デバイス (拡大表示の窓のスワップチェーンを作って描く)。無ければ拡大表示を開けない。
    void SetRenderTarget(ID2D1RenderTarget* render_target, ID3D11Device* d3d = nullptr, ID2D1Device* device = nullptr);
    // 本体の窓の DPR で今その場で描く (大きな図は画素数の上限まで縮める)。
    Microsoft::WRL::ComPtr<IWICBitmap> DiagramBitmap(const mermaid_util::DiagramDrawing& drawing, float max_width) override;
    // deadline を過ぎた分は残し、HasLeftoverJobResults が真になる。描画先が無い間は何もしないので、戻ったら呼ぶ。
    void ProcessJobResults(std::chrono::steady_clock::time_point deadline = std::chrono::steady_clock::now() + kImportSliceBudget);
    // 新しい結果は投函で知らせるので、続きが要るのは時間切れで残したときだけ。UI スレッド専用。
    bool HasLeftoverJobResults() const noexcept
    {
        return render_target_ && job_results_.HasLeftover();
    }
    void Shutdown();
    void DropQueued() override;
    void BeginRequestPass() override;
    void ReleaseDrawing(DiagramEntry& diagram_entry) override;
    // 覚えている結果のコマンドリストを全部外す (描画命令は残す)。文書を切り替えたら、前の文書の図のものはどの図も持たない。
    void ReleaseCachedCommands();
    // 覚えている結果のうち、コマンドリストを持つものの数 (テスト用)。
    size_t CachedCommandLists();
    // 描画の待ちの順。表示用は要求のキー、数式は 0 (テスト用)。
    std::vector<uint64_t> WaitingRequests() const;
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
        // 結果を width_free_hash で持つ。ディスクで当たったか、描いた結果が幅の上限より狭かったか、どの幅でも使える。
        bool width_free = false;
        // どの幅でも使える (DiagramResult::any_width)。width_free も立つ。
        bool any_width = false;
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
        // キャッシュにある描画命令の文字を組む。画面外でコマンドリストを捨てた図を記録し直す前に行う。表示用の要求だけ。
        Relayout,
    };
    // 描かず・読まずに返した。投入後に drop_gen_ が進んだ表示用の要求か、PeleEngine が取り消した (終了)。
    struct JobDropped {};
    // 描いたか読んだ (か文字を組み直した) 描画命令。UI で commands にする。
    struct JobDrawing {
        std::shared_ptr<const mermaid_util::DiagramDrawing> drawing;
        // drawing の文字の並びを worker で組んだもの。記録したら捨てる (キャッシュには持たない)。
        DiagramTextLayouts texts;
        // 描いたときだけ、ディスクキャッシュに書く描画命令 (UTF-8)。
        std::string bytes;
        // 描いたときだけ、pele の SVG のハッシュ (PeleOutcome::svg_hash)。
        uint64_t svg_hash = 0;
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
        // 投入したときの drop_gen_。表示用の仕事は、これと違えば描かずに返し、描いている途中でも止める。
        uint32_t drop_gen = 0;
        // PeleEngine のランタイムを使った (使っていないランタイムを捨てさせる)。
        bool used_pele = false;
    };

    // 待ちが空になって 1 個まで減らしたランタイムの最後の 1 個を、この時間使わなければ捨てる (読み進める間の切れ目では
    // 捨てない長さ)。別のアプリへ切り替わったときは待たずに捨てる。
    static constexpr UINT PELE_TRIM_MS = 10000;
    // エラーも含む。書きかけの図を保存するたびにエラーが増えても、古いものから捨てる。
    static constexpr size_t MAX_CACHE_ENTRIES = 128;
    // 数えるのは描画命令だけで、より大きい CommandList は数えない。CommandList は表示中の結果の分を DiagramEntry と共有し、
    // 置き換わった結果と画面外へ出た図のものは外す。CommandList も数える案は効きが小さいので見送った (ユーザーの判断)。
    // 1 回に届く結果より小さい上限にすると、描き直しが回り続ける。
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
    // 待ちの先頭に並ぶ数式の要求の数。数式は行の高さを決めるので、表示用の図より先に描く。
    size_t WaitingMathCount() const;
    // 待ちの表示用の要求を、この回に頼まれた分の後ろへ移す (BeginRequestPass)。
    void PromoteWaiting(uint64_t request_hash);
    // req から Job を作って bg で work(job) し、その結果を outcome に入れて job_results_ に返す
    template <typename Work>
    void PostJob(RenderRequest req, JobWork kind, Work work);
    // worker で呼ぶ。
    JobOutcome RenderJob(Job& job);

    void StartDiskLoad(RenderRequest req, std::filesystem::path path);
    // worker で呼ぶ。
    JobOutcome LoadDiskJob(Job& job, const std::filesystem::path& path);
    // 描画命令の文字を worker で組み、UI では組んだ文字で記録するだけにする (文字の組みが記録の大半を占める)。
    void StartRelayout(RenderRequest req, std::shared_ptr<const mermaid_util::DiagramDrawing> drawing);
    // 戻り値: 結果 (描画命令かエラー) が出たか。
    bool ApplyJobResult(Job& r);
    bool ApplyDisplayResult(Job& r);
    bool SettleDisplayError(const DisplayTarget& display, const DiagramError& error);

    // UI スレッドで呼ぶ。描画中の Renderer とは別の DeviceContext で記録する。記録できなければ null。
    Microsoft::WRL::ComPtr<ID2D1CommandList> RecordDrawing(const mermaid_util::DiagramDrawing& drawing, const DiagramTextLayouts* texts);
    void InsertCache(uint64_t hash, DiagramResult result);
    // commands を覚えている結果から外す。どの図も持たなくなったコマンドリストを残さない。
    void ForgetCommands(const Microsoft::WRL::ComPtr<ID2D1CommandList>& commands);
    // 幅を混ぜないキーの結果 (いまの幅の上限より狭いときだけ使える) を先に、無ければ幅つきのキーで引く。
    // target.width_free には、幅なしのキーで当たったかを書く。
    DiagramResult* FindCached(DisplayTarget& target, float max_width);
    // 結果 (描画命令かエラー) が出ていれば返す。まだなら読み込みか描画を始めて null (始めていれば何もしない)。
    // cached_key があれば、当たった結果のキーを書く (Find は並びを入れ替えるので、引き直さずにこれで同じ結果を指す)。
    DiagramResult* FindOrRequest(std::string_view code, float max_width, bool dark_mode, DisplayTarget* cached_key = nullptr);

    HWND hwnd_ = nullptr;
    ID2D1RenderTarget* render_target_ = nullptr;
    Microsoft::WRL::ComPtr<ID3D11Device> d3d_device_;
    // render_target_ のデバイス。拡大表示の窓はこれで描く。
    Microsoft::WRL::ComPtr<ID2D1Device> d2d_device_;
    Microsoft::WRL::ComPtr<ID2D1Factory> d2d_factory_;
    Microsoft::WRL::ComPtr<IWICImagingFactory> wic_factory_;

    ViewerFactory open_viewer_ = nullptr;
    Viewer viewer_;
    std::move_only_function<void()> on_viewer_failed_;

    std::move_only_function<void()> on_update_;

    std::unordered_set<uint64_t> inflight_;
    // 図のコードとテーマ (幅なしのキー) ごとに、最後に描いた幅と pele の SVG のハッシュ。別の幅で描き直して SVG が同じなら、
    // 幅で出力が変わらない図なので、どの幅でも使える結果として幅なしのキーに置く (追加の JS は要らない)。
    struct DrawnSvg {
        float max_width = 0.0f;
        uint64_t svg_hash = 0;
    };
    std::unordered_map<uint64_t, DrawnSvg> drawn_svgs_;

    TranspositionCache<uint64_t, DiagramResult, MAX_CACHE_ENTRIES> cache_;
    MermaidFileCache* file_cache_ = nullptr;

    TaskScheduler* bg_scheduler_ = nullptr;
    PostedInbox<Job> job_results_;
    std::atomic<uint32_t> drop_gen_{ 0 };

    PeleFactory make_pele_ = nullptr;
    std::unique_ptr<PeleEngine> pele_;
    // 数式の要求、この回 (BeginRequestPass) に頼まれた表示用の要求 (pass_count_ 個)、それより前の回の表示用の要求の順に並ぶ。
    std::deque<RenderRequest> render_waiting_;
    size_t pass_count_ = 0;
    int renders_running_ = 0;

    Microsoft::WRL::ComPtr<ID2D1DeviceContext5> record_dc_;
    Microsoft::WRL::ComPtr<IDWriteFactory> dw_factory_;

    WorkerLatch latch_;
};
