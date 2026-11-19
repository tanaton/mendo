#pragma once
#include "document_types.h"
#include "diagram_raster.h"
#include "diagram_view.h"
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
// 描けた図は直列化した描画命令 (SerializeDrawing) とラベルの数式の描画命令を持ち、表示している間だけ解いた描画命令と
// コマンドリストも持つ。画面外へ出た・置き換わった・前の文書の図など、どの図も拡大表示も持たなくなった解いた形は手放し
// (ReleaseIdleDrawings)、戻ったら worker で解き直す (JS は動かさない)。解いた形は直列化した形より大きいので、表示中の図の
// 分だけにする (ユーザーが選んだ)。表示中かどうかは持ち主の数で決め、表示が終わる経路ごとに知らせてもらわない。
// 設定ファイルのほかにファイルを書かないよう、ディスクには置かず RAM だけに持つ (ユーザーが選んだ)。起動し直すと空から始まる。
class DiagramCache {
public:
    // 直列化した描画命令・ラベルの数式・エラーの文言・項目の器を数える上限 (ユーザーが選んだ)。表示中の解いた形と
    // コマンドリストは数えず、表示中の項目は捨てない (画面に出ている図の数で決まる)。
    static constexpr size_t MAX_BYTES = 4u * 1024 * 1024;
    // 上限に数える、項目の器 (表の節・結果・共有の管理ブロック) の見込み。
    static constexpr size_t ITEM_BYTES = 256;
    // 上限に数える、ラベルの数式 1 つの描画命令の見込み (図のラベルの数式は短い)。
    static constexpr size_t LABEL_MATH_BYTES = 1024;

    struct Item {
        // 寸法・どの幅でも使えるか・エラー。表示している間は解いた描画命令 (drawing) とコマンドリストも。
        DiagramResult result;
        // 直列化した描画命令。エラーと、上限より大きい図は持たない。worker で解く間に捨てられても残るよう共有する。
        std::shared_ptr<const std::string> bytes;
        // ラベルの数式の描画命令 (層と数式の順)。直列化した形は TeX だけを持つので、解いた後に差し込み、KaTeX を動かさない。
        std::vector<std::shared_ptr<const MathDrawing>> maths;
    };
    struct Hit {
        uint64_t key = 0;
        Item* item = nullptr;
    };

    explicit DiagramCache(size_t max_bytes = MAX_BYTES) noexcept : max_bytes_(max_bytes)
    {}

    // 幅に縛られない結果 (max_width より狭いか、どの幅でも使えるときだけ) を先に、無ければ幅つきで引き、使ったことにする。
    // item は次に表を変えるまで使える。
    Hit Find(uint64_t width_free_key, uint64_t sized_key, float max_width);
    // Find と同じく引くが、寸法を見るだけなので捨てる順を変えない。
    const Item* Peek(uint64_t width_free_key, uint64_t sized_key, float max_width) const;
    // key の項目を、捨てる順を変えずに見る (テスト用)。
    const Item* Peek(uint64_t key) const;
    // 同じキーは置き換える (置くものが無ければ残す)。上限より大きい図は直列化した形を持たない。上限を超えても置くときには捨てず、次の
    // ReleaseIdleDrawings で捨てる。表示中の図だけで上限を超えていると、置いたばかりのエラーを図が受け取る前に捨て、
    // 同じ図を JS で描き直し続けてしまう。
    void Put(uint64_t key, Item item);
    void Erase(uint64_t key);
    // どの図も拡大表示も worker の仕事も持たなくなった (この表だけが持つ) 解いた描画命令とコマンドリストを外し、直列化した形も
    // エラーも無くなった項目は捨てる。その後、上限を超えていれば、表示していない項目を使っていない順に捨てる。
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
        Item item;
        // 上限に数える大きさ。
        size_t cost = 0;
        uint64_t last_used = 0;
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
    void Init(HWND hwnd, ID2D1Factory* d2d, IWICImagingFactory* wic, IDWriteFactory* dw, TaskScheduler& bg_scheduler, PeleFactory make_pele,
              ViewerFactory open_viewer, std::move_only_function<void()> on_update, std::move_only_function<void()> on_viewer_failed);

    bool RequestRender(const Node& node, DiagramEntry& diagram_entry, float max_width, bool dark_mode) override;
    std::optional<mermaid_util::DiagramSize> KnownSize(const Node& node, float max_width, bool dark_mode) const override;
    // インライン数式をまとめて bg のワーカーで描く (PeleEngine の KaTeX)。図と同じ同時に描く数の枠を使う。
    // 速いので図の結果のキャッシュには置かない。
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
        // キャッシュにある描画命令の文字を組む (解いた形が無ければ直列化した形から解く)。表示用の要求だけ。
        Load,
    };
    // 描かず・読まずに返した。投入後に drop_gen_ が進んだ表示用の要求か、PeleEngine が取り消した (終了)。
    struct JobDropped {};
    // 描いたか読んだ (か文字を組み直した) 描画命令。UI で commands にしてキャッシュに置く。
    struct JobDrawing {
        std::shared_ptr<const mermaid_util::DiagramDrawing> drawing;
        // drawing の文字の並びを worker で組んだもの。記録したら捨てる (キャッシュには持たない)。
        DiagramTextLayouts texts;
        // キャッシュに置く直列化した描画命令とラベルの数式 (DiagramCache::Item)。描いたときは worker で作り、読んだときは
        // キャッシュにあったものを引き継ぐ。
        std::shared_ptr<const std::string> bytes;
        std::vector<std::shared_ptr<const MathDrawing>> maths;
        // 描いたときだけ、bytes のハッシュ (last_drawn_)。
        uint64_t drawing_hash = 0;
    };
    // PeleEngine で描けなかった。描き直しても同じ。
    struct JobFailed {
        DiagramError error;
    };
    // キャッシュにある直列化した描画命令を解けなかった。同じプロセスで書いたものなので、直列化と解析が食い違ったときの保険。
    struct JobUnreadable {};
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
    };

    // 待ちが空になって 1 個まで減らしたランタイムの最後の 1 個を、この時間使わなければ捨てる (読み進める間の切れ目では
    // 捨てない長さ)。別のアプリへ切り替わったときは待たずに捨てる。
    static constexpr UINT PELE_TRIM_MS = 10000;
    // 前の幅の記録 (last_drawn_) の件数の上限。保存のたびにコードが変わっても増え続けないよう、図の結果のキャッシュに入りうる
    // 件数に合わせる (ユーザーが選んだ)。溢れたら全部捨てる (描き直しを省く手掛かりなので、次に別の幅で描けば覚え直す)。
    static constexpr size_t MAX_LAST_DRAWN = DiagramCache::MAX_BYTES / DiagramCache::ITEM_BYTES;

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
    // bg のワーカーで PeleEngine に描かせ、結果をキャッシュの読み込みと同じ経路で返す。
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

    // item の描画命令の文字を worker で組み、UI では組んだ文字で記録するだけにする (文字の組みが記録の大半を占める)。
    // 解いた形が無ければ、先に直列化した形を解いてラベルの数式を差し込む。
    void StartLoad(RenderRequest req, const DiagramCache::Item& item);
    // 戻り値: 結果 (描画命令かエラー) が出たか。
    bool ApplyJobResult(Job& r);
    bool ApplyDisplayResult(Job& r);
    bool SettleDisplayError(const DisplayTarget& display, const DiagramError& error);

    // UI スレッドで呼ぶ。描画中の Renderer とは別の DeviceContext で記録する。記録できなければ null。
    Microsoft::WRL::ComPtr<ID2D1CommandList> RecordDrawing(const mermaid_util::DiagramDrawing& drawing, const DiagramTextLayouts* texts);
    // 結果 (解いた描画命令かエラー) が出ていれば返す。まだなら読み込みか描画を始めて null (始めていれば何もしない)。
    // cached_key があれば、当たった結果のキーを書く。
    DiagramCache::Item* FindOrRequest(std::string_view code, float max_width, bool dark_mode, DisplayTarget* cached_key = nullptr);

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
    // 幅の上限より広いフローチャートのコードとテーマ (幅なしのキー) ごとに、最後に描いた要求のキー (幅を含む) と描画命令の
    // ハッシュ。別の幅で描き直してハッシュが同じなら、幅で出力が変わらない図なので、どの幅でも使える結果として幅なしのキーに
    // 置く (追加の JS は要らない)。前の幅の結果がキャッシュから捨てられても、上限より大きくて直列化した形を持たなくても比べられる
    // よう、ハッシュで覚える。描くたびに引くので開番地法の表で持ち、キャッシュの上限には数えない (どちらも速さをユーザーが選んだ)。
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
