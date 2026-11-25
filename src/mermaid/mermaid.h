#pragma once
#include "document_types.h"
#include "diagram_raster.h"
#include "inline_math.h"
#include "layout_cache.h"
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
#include <functional>
#include <deque>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <variant>
#include <vector>

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

// 図の結果のキャッシュ。キーは DiagramRequestHash か DiagramWidthFreeHash。UI スレッドから使う。
// 描けた図は描画命令 (DiagramDrawing) を持ち、表示している間だけコマンドリストも持つ。どの図も拡大表示も持たなくなったら
// コマンドリストを手放し (ReleaseIdleDrawings)、戻ったら worker で文字を組んで記録し直す (JS は動かさない)。表示中かどうかは
// 描画命令の持ち主の数で決め、表示が終わる経路ごとに知らせてもらわない。
// 描画命令は画面外でも詰めた 1 つの形で持つ。表示中の二重持ちと戻るときの解き直しが無い。文字列に
// 直して持つより、長い英字・数式ラベルの図と極小の図は大きい。
// ファイルを書かないよう (設定はレジストリ)、ディスクには置かず RAM だけに持つ。起動し直すと空から始まる。
class DiagramCache {
public:
    // 描画命令・ラベルの数式・エラーの文言・項目の器を数える上限。コマンドリストは数えず、表示中の項目は
    // 捨てない (画面に出ている図の数で決まる)。
    static constexpr size_t MAX_BYTES = 4u * 1024 * 1024;
    // 上限に数える、項目の器 (表の節・結果・共有の管理ブロック) の見込み。
    static constexpr size_t ITEM_BYTES = 256;
    // 上限に数える、ラベルの数式 1 つの描画命令の見込み (図のラベルの数式は短い)。
    static constexpr size_t LABEL_MATH_BYTES = 1024;

    struct Hit {
        uint64_t key = 0;
        const DiagramResult* result = nullptr;
    };

    explicit DiagramCache(size_t max_bytes = MAX_BYTES) noexcept : max_bytes_(max_bytes)
    {}

    // 幅に縛られない結果 (max_width より狭いか、どの幅でも使えるときだけ) を先に、無ければ幅つきで引き、使ったことにする。
    // result は次に表を変えるまで使える。
    Hit Find(uint64_t width_free_key, uint64_t sized_key, float max_width);
    // Find と同じく引くが、寸法を見るだけなので捨てる順を変えない。
    const DiagramResult* Peek(uint64_t width_free_key, uint64_t sized_key, float max_width) const;
    // key の項目を、捨てる順を変えずに見る (テスト用)。
    const DiagramResult* Peek(uint64_t key) const;
    // 描画命令かエラーのある結果を置く。同じキーは置き換える。上限より大きい図は表示している間だけ持つ。上限を超えても置くときには
    // 捨てず、次の ReleaseIdleDrawings で捨てる。表示中の図だけで上限を超えていると、置いたばかりのエラーを図が受け取る前に
    // 捨て、同じ図を JS で描き直し続けてしまう。
    void Put(uint64_t key, DiagramResult result);
    void Erase(uint64_t key);
    // どの図も拡大表示も worker の仕事も持たなくなった (描画命令をこの表だけが持つ) 図のコマンドリストを外し、上限より大きい図は捨てる。
    // その後、上限を超えていれば、表示していない項目を使っていない順に捨てる。
    void ReleaseIdleDrawings();
    // 描画デバイスが変わった。旧デバイスのコマンドリストと、旧デバイスで確定したかもしれないエラーを捨てる。
    void ResetDevice();

    // ここから 3 つはテスト用。
    size_t EntryCount() const noexcept
    {
        return slots_.size();
    }
    // 上限に数えている大きさ。
    size_t TotalBytes() const noexcept
    {
        return total_bytes_;
    }
    size_t CommandLists() const noexcept;

private:
    struct Slot {
        DiagramResult result;
        // 上限に数える大きさ。
        size_t cost = 0;
        uint64_t last_used = 0;
        // 上限より大きいので、表示している間だけ持つ。
        bool oversized = false;

        // 描画命令の持ち主が表の外にいる (表示中か worker が組んでいる)。持ち主を増やすのは UI スレッドだけなので、false と
        // 読めたら持ち主はこの表だけ (worker が手放す途中なら true と読み、次の回に外す)。
        bool HeldOutside() const noexcept
        {
            return result.drawing.use_count() > 1;
        }
    };
    using Slots = std::unordered_map<uint64_t, Slot>;
    // 次の項目を返す。
    Slots::iterator Erase(Slots::iterator it) noexcept;
    void Trim();

    // 捨てるのは上限を超えたときだけなので、順序の構造を持たず、捨てるときに last_used で古い順に並べる (Trim)。
    Slots slots_;
    size_t total_bytes_ = 0;
    uint64_t lru_seq_ = 0;
    size_t max_bytes_;
};

// Mermaid の図を描画命令にして ID2D1CommandList に記録する。図は bg のワーカーで pele を QuickJS で動かして描き (PeleEngine)、
// 拡大表示 (ZoomViewer) と保存・コピーの画像もその描画命令から作る。図のラベルの数式とインライン数式は同じランタイムの
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

    // App に頼むこと。窓に投げるメッセージやタイマーの番号は App が決め、ここでは知らない。wake のほかは空にしない。
    struct Hooks {
        // 待っていた図と、拡大表示に出す図を要求し直させる。UI スレッドで呼ぶ。
        std::move_only_function<void()> on_update;
        // 結果が溜まり始めたときに worker で呼ぶ。UI スレッドに ProcessJobResults を呼ばせるだけにする。空なら呼ばない。
        WorkerWake wake;
        // ms 後に TrimIdleRuntimes を呼ばせる。掛け直すとそこから数え直す。
        std::move_only_function<void(UINT ms)> set_trim_timer;
        std::move_only_function<void()> kill_trim_timer;
    };

    // d2d・wic・dw は Renderer と共有し、保存・コピーの画像の描画先 (d2d) と画像 (wic)、図の文字 (dw) を作るのに使う。
    // hwnd は本体の窓の DPI を読むのに使う。
    void Init(HWND hwnd, ID2D1Factory* d2d, IWICImagingFactory* wic, IDWriteFactory* dw, TaskScheduler& bg_scheduler, PeleFactory make_pele,
              Hooks hooks);

    bool RequestRender(const Node& node, DiagramEntry& diagram_entry, float max_width, bool dark_mode) override;
    std::optional<mermaid_util::DiagramSize> KnownSize(const Node& node, float max_width, bool dark_mode) const override;
    // インライン数式をまとめて bg のワーカーで描く (PeleEngine の KaTeX)。図と同じ同時に描く数の枠を使う。
    // 速いので図の結果のキャッシュには置かない。
    void RequestMathBatch(std::vector<InlineMathItem> items, MathBatchCallback callback);
    void SetRenderTarget(ID2D1RenderTarget* render_target);
    // 本体の窓の DPR で今その場で描く (大きな図は画素数の上限まで縮める)。
    Microsoft::WRL::ComPtr<IWICBitmap> DiagramBitmap(const mermaid_util::DiagramDrawing& drawing, float max_width) override;
    // deadline を過ぎた分は残し、HasLeftoverJobResults が真になる。描画先が無い間は何もしないので、戻ったら呼ぶ。
    void ProcessJobResults(std::chrono::steady_clock::time_point deadline = std::chrono::steady_clock::now() + kImportSliceBudget);
    // 新しい結果は Hooks::wake で知らせるので、続きが要るのは時間切れで残したときだけ。UI スレッド専用。
    bool HasLeftoverJobResults() const noexcept
    {
        return render_target_ && job_results_.HasLeftover();
    }
    void Shutdown();
    void DropQueued() override;
    void BeginRequestPass() override;
    void ReleaseIdleDrawings() override;
    // 描画の待ちの順。表示用は要求のキー、数式は 0 (テスト用)。
    std::vector<uint64_t> WaitingRequests() const;
    // 図の結果のキャッシュ (テストが中を見て、置いて読ませる)。
    DiagramCache& ResultCache() noexcept
    {
        return cache_;
    }
    // 描画待ちのインライン数式の要求を取り消す。数式の寸法表を捨てたときに、もう使わない組を描かせない。
    void DropQueuedMath();
    // PeleEngine の使っていないランタイムを捨てる。Hooks::set_trim_timer の時間が来たときと、別のアプリへ切り替わったときに呼ぶ。
    void TrimIdleRuntimes();

    // 結果 (記録した図かエラー) が出ていれば返す。まだなら文字の組みか描画を始めて null (始めていれば何もしない。結果が出たら
    // on_update で知らせる)。拡大表示 (ZoomViewer) も本文と同じ結果 (cache_) をここで引く (同じ図を本文用と 2 回描かない。本文と
    // 同じ折り返しになる)。戻り値は次にキャッシュを変えるまで使える。
    const DiagramResult* FindOrRequest(std::string_view code, float max_width, bool dark_mode);

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
        // 結果を width_free_hash で持つ。キャッシュで当たったか、描いた結果が幅の上限より狭かったか、どの幅でも使える。
        bool width_free = false;
        // どの幅でも使える (DiagramResult::any_width)。width_free も立つ。
        bool any_width = false;
        static DisplayTarget Of(uint64_t code_hash, float max_width, bool dark_mode) noexcept
        {
            return { .request_hash = mermaid_util::DiagramRequestHash(code_hash, max_width, dark_mode),
                     .width_free_hash = mermaid_util::DiagramWidthFreeHash(code_hash, dark_mode) };
        }
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

    enum class JobWork : uint8_t {
        // PeleEngine で描く。同時に描く数の枠を 1 つ使い、結果を受け取ったときに返す (PumpRenders)。
        Render,
        // キャッシュにある描画命令の文字を組む。表示用の要求だけ。
        Layout,
    };
    // 描かず・組まずに返した。投入後に drop_gen_ が進んだ表示用の要求か、PeleEngine が取り消した (終了)。
    struct JobDropped {};
    // 描いたか、キャッシュにあった描画命令。UI で commands にしてキャッシュに置く。
    struct JobDrawing {
        std::shared_ptr<const mermaid_util::DiagramDrawing> drawing;
        // drawing の文字の並びを worker で組んだもの。記録したら捨てる (キャッシュには持たない)。
        DiagramTextLayouts texts;
        // 描いたときだけ、描画命令のハッシュ (last_drawn_)。
        uint64_t drawing_hash = 0;
    };
    // PeleEngine で描けなかった。描き直しても同じ。
    struct JobFailed {
        DiagramError error;
    };
    // MathRequest::items と同じ順で、描けなかった数式は null。
    struct JobMaths {
        std::vector<std::shared_ptr<const MathDrawing>> drawings;
    };
    using JobOutcome = std::variant<JobDropped, JobDrawing, JobFailed, JobMaths>;

    // bg のワーカーの仕事 1 つとその結果。
    struct Job {
        RenderRequest req;
        // 既定値を置かない (Render だと指定し忘れた仕事も描画の枠を返す)。
        JobWork work;
        JobOutcome outcome;
        // 表示用の仕事だけに入れる、投入したときの drop_gen_。変わっていれば描かずに返し、描いている途中でも止める。
        PeleEngine::StaleWhen stale;
    };

    // 待ちが空になって 1 個まで減らしたランタイムの最後の 1 個を、この時間使わなければ捨てる (読み進める間の切れ目では
    // 捨てない長さ)。別のアプリへ切り替わったときは待たずに捨てる。
    static constexpr UINT PELE_TRIM_MS = 10000;
    // 前の幅の記録 (last_drawn_) の件数の上限。保存のたびにコードが変わっても増え続けないよう、図の結果のキャッシュに入りうる
    // 件数に合わせる。溢れたら全部捨てる (描き直しを省く手掛かりなので、次に別の幅で描けば覚え直す)。
    static constexpr size_t MAX_LAST_DRAWN = DiagramCache::MAX_BYTES / DiagramCache::ITEM_BYTES;

    void ApplyMathResult(Job& r);

    // bg のワーカーで PeleEngine に描かせ、結果を StartLayout と同じ経路で返す。
    void StartRender(RenderRequest req);
    // 同時に描く数の上限まで、待ちから bg へ出す。
    void PumpRenders();
    // 待ちの表示用の要求を、この回に頼まれた分の後ろへ移す (BeginRequestPass)。
    void PromoteWaiting(uint64_t request_hash);
    // req から Job を作って bg で work(job) し、その結果を outcome に入れて job_results_ に返す
    template <typename Work>
    void PostJob(RenderRequest req, JobWork kind, Work work);
    // worker で呼ぶ。
    JobOutcome RenderJob(Job& job);

    // drawing の文字を worker で組み、UI では組んだ文字で記録するだけにする (文字の組みが記録の大半を占める)。
    void StartLayout(RenderRequest req, std::shared_ptr<const mermaid_util::DiagramDrawing> drawing);
    // 戻り値: 結果 (記録した図かエラー) が出たか。
    bool ApplyDisplayResult(Job& r);
    bool SettleDisplayError(const DisplayTarget& display, const DiagramError& error);

    HWND hwnd_ = nullptr;
    ID2D1RenderTarget* render_target_ = nullptr;
    Microsoft::WRL::ComPtr<ID2D1Factory> d2d_factory_;
    Microsoft::WRL::ComPtr<IWICImagingFactory> wic_factory_;

    Hooks hooks_;

    std::unordered_set<uint64_t> inflight_;
    // 幅の上限より広いフローチャートのコードとテーマ (幅なしのキー) ごとに、最後に描いた要求のキー (幅を含む) と描画命令の
    // ハッシュ。別の幅で描き直してハッシュが同じなら、幅で出力が変わらない図なので、どの幅でも使える結果として幅なしのキーに
    // 置く (追加の JS は要らない)。前の幅の結果は捨てられていることがある (上限を超えたか、上限より大きい図を手放した) ので、
    // ハッシュで覚えて比べる。描くたびに引くので開番地法の表で持ち、キャッシュの上限には数えない (どちらも速さを選んだ)。
    struct LastDrawn {
        uint64_t request_hash = 0;
        uint64_t drawing_hash = 0;
    };
    FlatHashMap<LastDrawn> last_drawn_;

    DiagramCache cache_;

    TaskScheduler* bg_scheduler_ = nullptr;
    PostedInbox<Job> job_results_;
    std::atomic<uint32_t> drop_gen_{ 0 };

    PeleFactory make_pele_ = nullptr;
    std::unique_ptr<PeleEngine> pele_;
    // 描画の待ち。数式の要求は表示用の図より先に描く (PumpRenders)。表示用の要求は、この回 (BeginRequestPass) に頼まれた
    // もの (先頭の pass_count_ 個)、それより前の回のものの順に並ぶ。
    std::deque<RenderRequest> math_waiting_;
    std::deque<RenderRequest> display_waiting_;
    size_t pass_count_ = 0;
    int renders_running_ = 0;

    Microsoft::WRL::ComPtr<ID2D1DeviceContext5> record_dc_;
    Microsoft::WRL::ComPtr<IDWriteFactory> dw_factory_;

    WorkerLatch latch_;
};
