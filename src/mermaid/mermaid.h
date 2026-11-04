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
    // 描く前に取り消した (終了・文書の差し替え)。items の metrics はすべて空。数式のエラーではないので、後で頼み直してよい。
    bool cancelled = false;
};
// 描けたとき・失敗したとき・取り消したときのどれでも 1 回だけ呼ぶ。
using MathBatchCallback = std::move_only_function<void(InlineMathBatchResult result)>;

// Mermaid の図を描画命令にして ID2D1CommandList に記録する。図は bg のワーカーで pele を QuickJS で動かして描き
// (PeleEngine)、拡大表示 (DiagramView) とクリップボードの SVG もその描画命令から作る。図のラベルの数式とインライン数式は
// 同じランタイムで KaTeX を動かして描く。描画命令にできない図は描かず、描けないことを知らせる。
// すべてのパブリックメソッドはUIスレッドから呼び出す必要がある。
class MermaidRenderer : public IMermaidRenderer {
public:
    MermaidRenderer() = default;
    ~MermaidRenderer() override;

    MermaidRenderer(const MermaidRenderer&) = delete;
    MermaidRenderer& operator=(const MermaidRenderer&) = delete;

    // wic は Renderer と共有する PNG エンコード用。
    // bg_scheduler は描画とディスクキャッシュの読み込みを UI スレッド外で行う先で、
    // 完了は app_msg::MERMAID_DISK_LOADED で hwnd に通知され、受信側が ProcessDiskLoads を呼ぶ。
    // on_update は結果が出たときに呼ぶ。結果を待つ図を要求し直させる。
    // on_viewer_failed は拡大表示を開けなかったときに、ウィンドウを閉じてから呼ぶ。
    void Init(HWND hwnd, IWICImagingFactory* wic, MermaidFileCache& file_cache, TaskScheduler& bg_scheduler,
              std::move_only_function<void()> on_update, std::move_only_function<void()> on_viewer_failed);

    bool RequestRender(const Node& node, DiagramEntry& diagram_entry, float max_width, bool dark_mode) override;
    void RequestSvg(std::string_view code, float max_width, bool dark_mode, SvgCallback callback) override;
    // インライン数式をまとめて bg のワーカーで描く (PeleEngine の KaTeX。1 個 平均 0.2ms ほど)。図と同じ同時に描く数の枠を使う。
    // 速いのでディスクキャッシュには置かない。
    void RequestMathBatch(std::vector<InlineMathItem> items, MathBatchCallback callback);
    void SetRenderTarget(ID2D1RenderTarget* render_target);
    // 図の保存・コピーに使う PNG。本体の窓の DPR で今その場で描く (大きな図は画素数の上限まで縮める)。作れなければ null。
    PngBytes DiagramPng(const DiagramEntry& entry);
    // deadline を過ぎた分は残し、HasLeftoverDiskLoads が真になる。描画先が無い間は何もしないので、戻ったら呼ぶ。
    void ProcessDiskLoads(std::chrono::steady_clock::time_point deadline = std::chrono::steady_clock::now() + kUploadSliceBudget);
    // 新しい結果は投函で知らせるので、続きが要るのは時間切れで残したときだけ。UI スレッド専用。
    bool HasLeftoverDiskLoads() const noexcept
    {
        return render_target_ && disk_results_.HasLeftover();
    }
    void Shutdown();
    void DropQueued() override;
    // 描画待ちのインライン数式の要求を取り消す (cancelled で返す)。数式の寸法表を捨てたときに、もう使わない組を描かせない。
    void DropQueuedMath();
    // PeleEngine の使っていないランタイムを捨てる。
    void OnPeleTrimTimer();

    // 図を別ウィンドウ (1 枚だけ) に開く。開いていれば中身を差し替える。code (UTF-8) は内部でコピーする。
    void OpenViewer(std::string_view code, float max_width, bool dark_mode);
    void CloseViewer();
    // 開いていれば、倍率と位置を保って新しいテーマの図に差し替える。
    void SetViewerDarkMode(bool dark_mode);
    // 開いている図のコード。閉じていれば nullopt (図を空にして保存すると、開いていてもコードは空になる)。
    std::optional<std::string_view> ViewerCode() const noexcept
    {
        return viewer_.hwnd ? std::optional<std::string_view>{ viewer_.code } : std::nullopt;
    }
    // 保存で図が変わったら、位置とボタンで変えた倍率を保って差し替える。描けなければ今の図を残す。開いていない・同じコードなら何もしない。
    void UpdateViewerCode(std::string_view code);
    void ApplyViewerLanguage();

private:
    struct MathRequest {
        std::vector<InlineMathItem> items;
        MathBatchCallback callback;
    };

    // Display だけが結果をキャッシュに持つ。Svg は SVG文字列を svg_callback で、View は描画命令を drawing_callback で、
    // Math は結果を math->callback で返す。
    enum class RequestKind : uint8_t { Display, Svg, View, Math };

    struct RenderRequest {
        // UTF-8 の原文。呼び出し側の文字列の寿命から切り離して持つ。
        std::string code;
        float max_width = 0.0f;
        bool dark_mode = false;
        // 結果を width_free_hash で持つ。ディスクで当たったか、描いた結果が幅の上限より狭かった。
        bool width_free = false;
        // 幅つきのキー。要求中の判定とエラー結果はこちらで持つ。
        uint64_t request_hash = 0;
        uint64_t width_free_hash = 0;
        uint64_t CacheKey() const noexcept
        {
            return width_free ? width_free_hash : request_hash;
        }
        RequestKind kind = RequestKind::Display;
        SvgCallback svg_callback;
        // 拡大表示の要求だけが持つ。描けなければ drawing は null。
        std::move_only_function<void(std::shared_ptr<const mermaid_util::DiagramDrawing> drawing)> drawing_callback;
        // Math の要求だけが持つ (RequestMathBatch)。code などは使わない。
        std::unique_ptr<MathRequest> math;
    };

    // 図の拡大表示ウィンドウ。描画命令を DiagramView で描く。
    // PopupWindow はキャプチャを取り、ホイールを本体へ渡し、非アクティブで閉じるので、見ながら操作するこの窓には使えない。
    struct Viewer {
        HWND hwnd = nullptr;
        std::unique_ptr<DiagramView> view;
        // 最後に要求した図。描けなかったコードも入るので、出ている絵のコードとは限らない。
        std::string code;
        float max_width = 0.0f;
        bool dark_mode = false;
        // この図を出した。以後は描き直しに失敗しても窓を閉じない。
        bool has_image = false;
    };

    struct DiskLoad {
        RenderRequest req;
        // インライン数式の要求の結果。MathRequest::items と同じ順で、描けなかった数式は null。
        std::vector<std::shared_ptr<const MathDrawing>> maths;
        // 描いたばかりの図の、ディスクキャッシュに書く描画命令 (UTF-8)。
        PngBytes bytes;
        // 描画命令。UI で commands にする。
        std::shared_ptr<const mermaid_util::DiagramDrawing> parsed;
        // ディスクキャッシュを読めなかった。理由は read_error。
        bool read_failed = false;
        DWORD read_error = 0;
        // 投入後に disk_drop_gen_ が進んだので読まずに返した。
        bool dropped = false;
        // PeleEngine で描いた (ディスクキャッシュから読んだのではない)。成功はディスクキャッシュへ書き、失敗は確定させる。
        // 走っている数も数える。
        bool native = false;
        // 描けなかった理由。描画命令を記録できなかったときは空。
        DiagramError error;
        // PeleEngine のランタイムを使った (使っていないランタイムを捨てさせる)。
        bool used_pele = false;
        // クリップボード用に PeleEngine で作った SVG。
        std::wstring svg;
    };

    // ランタイムは 5ms ほどで作り直せる。読み進める間の描画の切れ目では捨てない長さにする。
    static constexpr UINT PELE_TRIM_MS = 10000;
    // エラーも含む。書きかけの図を保存するたびにエラーが増えても、古いものから捨てる。
    static constexpr size_t MAX_CACHE_ENTRIES = 128;
    // ディスクキャッシュから戻せるので画像より控えめにする。
    static constexpr size_t MAX_CACHE_BYTES = 64u * 1024 * 1024;

    // SVG 専用リクエストならコールバックを呼んで svg_callback をクリアする。
    static void InvokeSvgCallbackIfAny(RenderRequest& req, std::wstring_view svg, bool cancelled);
    // インライン数式の要求なら、取り消したことを知らせて math->callback をクリアする。
    static void CancelMathCallbackIfAny(RenderRequest& req);
    // 結果を返さずに終える要求のコールバック (SVG・インライン数式) を呼ぶ。SVG は失敗か取り消し、インライン数式は
    // いつも取り消し。成功した要求は呼び済みで何もしない。
    static void FailCallbacksIfAny(RenderRequest& req, bool cancelled)
    {
        InvokeSvgCallbackIfAny(req, {}, cancelled);
        CancelMathCallbackIfAny(req);
    }
    void ApplyMathLoad(DiskLoad& r);
    // queue から kind の要求を順を保って取り出す。
    // 後始末は呼び出し側が取り出した後で行う (コールバックから描画待ちに積み直しても壊れない)。
    static std::vector<RenderRequest> TakePending(std::deque<RenderRequest>& queue, RequestKind kind);
    // 表示用リクエストのエラーを kind で確定させる (issue #271)。msg は pele の文言で、空なら汎用の文言を出す。
    void SetDiagramError(const RenderRequest& req, DiagramError::Kind kind, std::wstring_view msg = {});
    static void ApplyResult(DiagramEntry& diagram_entry, const DiagramResult& result);

    static LRESULT CALLBACK ViewerWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
    bool CreateViewer();
    // 描けた図を出したら drawn。描けなければ、図が出ていなければ閉じる。
    void OnViewerResult(bool drawn);
    void RequestViewerDrawing();
    // 今までの要求の結果を要らなくする。描画中の分は世代で捨て、待ちの分は描かせない (結果を待つ人がいないのでコールバックは
    // 呼ばない)。新しい世代を返す。
    unsigned int InvalidateViewerDrawing()
    {
        std::erase_if(native_waiting_, [](const RenderRequest& r) { return r.kind == RequestKind::View; });
        return ++viewer_render_;
    }
    void DestroyViewer();
    void FailViewer();

    // 最初に図か数式を描くときに作る。bg のワーカーが使うので、UI スレッドで投げる前に呼ぶ。
    void EnsurePele();
    // bg のワーカーで PeleEngine に描かせ、結果をディスクキャッシュの読み込みと同じ経路で返す。
    void StartNativeRender(RenderRequest req);
    // 同時に描く数の上限まで、待ちから bg へ出す。
    void PumpNativeRenders();
    // job を bg で work(job) してから disk_results_ に返す。投入後に disk_drop_gen_ が進んだ表示用の要求は work せずに返す。
    template <typename Work>
    void PostDiskJob(DiskLoad job, Work work);
    // worker で呼ぶ。
    void RenderNativeJob(DiskLoad& job, std::wstring_view locale);
    void ReleaseInflight(const RenderRequest& req) noexcept
    {
        if (req.kind == RequestKind::Display) {
            inflight_.erase(req.request_hash);
        }
    }

    void StartDiskLoad(RenderRequest req, std::filesystem::path path);
    // worker で呼ぶ。locale は図のラベルの数式を組み直すのに使う。
    void LoadDiskJob(DiskLoad& job, const std::filesystem::path& path, std::wstring_view locale);
    // 戻り値: 結果 (描画命令かエラー) が出たか。
    bool ApplyDiskLoad(DiskLoad& r);

    // UI スレッドで呼ぶ。描画中の Renderer とは別の DeviceContext で記録する。記録できなければ null。
    Microsoft::WRL::ComPtr<ID2D1CommandList> RecordDrawing(const mermaid_util::DiagramDrawing& drawing);
    // 図の文字を組む共有ファクトリ。図の無い文書の起動で読み込まないよう、最初に使うときに作る。
    IDWriteFactory* WriteFactory();
    void InsertCache(uint64_t hash, DiagramResult result);
    const DiagramResult* FindCached(uint64_t request_hash, uint64_t width_free_hash, float max_width);

    HWND hwnd_ = nullptr;
    ID2D1RenderTarget* render_target_ = nullptr;
    Microsoft::WRL::ComPtr<IWICImagingFactory> wic_factory_;

    Viewer viewer_;
    // 拡大表示の要求ごとに進め、新しい要求か閉じた後に届いた古い結果を捨てる。
    unsigned int viewer_render_ = 0;
    std::move_only_function<void()> on_viewer_failed_;

    // 結果が出たときに呼ぶ。
    std::move_only_function<void()> on_update_;

    // 待機中・読み込み中・描画中の表示用リクエストのコードハッシュ。未完了の図はスクロールのたびに
    // 再要求されるため、ここで重複を弾く (二重描画・キュー肥大の防止)。
    std::unordered_set<uint64_t> inflight_;

    // キャッシュキー -> 描画結果 (挙動は src/util/transposition_cache.h 参照)。同じコードは同じ結果になるので、失敗も成功と
    // 同じく覚えて描き直さない。
    TranspositionCache<uint64_t, DiagramResult, MAX_CACHE_ENTRIES> cache_;
    MermaidFileCache* file_cache_ = nullptr;

    TaskScheduler* bg_scheduler_ = nullptr;
    PostedInbox<DiskLoad> disk_results_;
    // DropQueued / Shutdown で進め、投入済みのディスクキャッシュの読み込みを取り消す (ImageLoader の drop_gen_ と同じ)。
    std::atomic<uint32_t> disk_drop_gen_{ 0 };

    // 最初に図を描くときに作る。終了時は描き終わりを待ってから捨てる。
    std::unique_ptr<PeleEngine> pele_;
    // PeleEngine で描く要求のうち、上限を超えて待っているものと、bg で走っている数。
    std::deque<RenderRequest> native_waiting_;
    int native_running_ = 0;

    // 描画命令を記録する DeviceContext。描画先と同じデバイスから作り、描画先が変わったら作り直す。
    Microsoft::WRL::ComPtr<ID2D1DeviceContext5> record_dc_;
    Microsoft::WRL::ComPtr<IDWriteFactory> dw_factory_;

    // Shutdown で worker 完了を待つ。scheduler 共有 worker から self を参照する race を排除する。
    WorkerLatch latch_;
};
