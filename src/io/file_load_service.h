#pragma once
#include "document_service.h"
#include "document.h"
#include "file_loader.h"
#include "layout_cache.h"
#include "task_scheduler.h"
#include <string>
#include <optional>
#include <expected>
#include <mutex>
#include <atomic>
#include <condition_variable>
#include <memory>
#include <thread>

struct Theme;

// ワーカースレッドでのパース結果。heights_estimated=false は preload (Theme 不在) から
// 来たケースで、UI スレッド側で EstimateNodeHeights を補完する必要がある。
struct AsyncLoadResult {
    Document doc;
    LayoutCache cache;
    bool heights_estimated = true;
};

// ファイル読み込みのオーケストレーションとローディングアニメーション状態を管理。
class FileLoadService {
public:
    explicit FileLoadService(DocumentService& doc_service) noexcept : doc_service_(doc_service)
    {}

    ~FileLoadService();

    // ---- ローディングアニメーション状態 ----

    constexpr bool IsLoading() const noexcept
    {
        return loading_;
    }
    // スピナー非表示でも非同期パースが進行中なら true。ライブリロードのバースト時に
    // 重複スケジューリングを抑制するためのフラグ。
    constexpr bool IsAsyncLoading() const noexcept
    {
        return async_in_flight_;
    }
    constexpr float GetLoadingAngle() const noexcept
    {
        return loading_angle_;
    }

    void StartLoading(std::pmr::wstring path);
    // path が既にセット済みの状態でアニメーションだけ起動する。preload 経由で
    // loading_path_ を二重代入したくないケース用。
    void BeginLoadingAnimation() noexcept;
    void StopLoading() noexcept;
    void TickLoadingAnimation() noexcept;

    // ---- ファイル読み込み ----

    std::expected<void, FileLoadError> ExecuteLoad(Document& doc, LayoutCache& cache);

    // ---- 非同期ファイル読み込み ----

    void StartAsyncLoad(TaskScheduler& scheduler, HWND hwnd, UINT msg_id, const Theme& theme);
    std::optional<AsyncLoadResult> TakeAsyncResult();
    // 直近の非同期ロードが返したエラー (失敗時のみ有効)。
    // OnParseComplete の null パスで取り出してトースト表示に使う。
    std::optional<FileLoadError> TakeAsyncError() noexcept;
    void CancelAsyncLoad() noexcept
    {
        async_gen_.fetch_add(1, std::memory_order_relaxed);
        async_in_flight_ = false;
    }

    // ---- 起動時 preload (App::Init 前から走らせる経路) ----

    // hwnd なしで I/O + パースをワーカースレッドに投入する。
    // EstimateNodeHeights は Theme 依存なので skip し、heights_estimated=false で
    // OnParseComplete に流す。
    void StartPreloadAsync(std::pmr::wstring path);

    enum class PreloadAttachResult {
        None,           // preload 未起動 → 呼び出し側は何もしない
        AppliedSync,    // 同期取り込み完了 → 呼び出し側は OnParseComplete 相当を実行
        AttachedAsync,  // worker に hwnd 通知済み → 呼び出し側は必要なら anim を起動
    };

    // App::Init 末尾で呼ぶ。preload の状態に応じて以下のいずれかを行う:
    //   完了済み: JoinPreload (= AppliedSync)。呼び出し側で OnParseComplete を発火。
    //   未完了:   worker に hwnd を渡して PostMessage を解禁 (= AttachedAsync)。
    PreloadAttachResult AttachOrApplyPreload(HWND hwnd, UINT msg_id);

    // ---- パスアクセス ----

    constexpr const std::pmr::wstring& GetLoadingPath() const noexcept
    {
        return loading_path_;
    }
    constexpr void SetLoadingPath(std::pmr::wstring path) noexcept
    {
        loading_path_ = std::move(path);
    }

private:
    // worker は cv.wait で hwnd セットを待ってから PostMessage する。
    // main 側の早期終了時は aborted=true + cv.notify_all でデッドロックを避ける。
    struct PreloadContext {
        std::mutex mtx;
        std::condition_variable cv;
        HWND hwnd = nullptr;
        UINT msg_id = 0;
        bool aborted = false;

        void SignalAbort();
    };

    // worker が I/O+パースを終えて async_result_/async_error_ に書き込み済みか判定。
    bool IsPreloadDone();
    // worker を abort 通知で停止させ join する。preload_ctx_ も解放する。
    void JoinPreload();
    // worker に hwnd を渡し PostMessage を解禁する。
    void OnInitComplete(HWND hwnd, UINT msg_id);

    // 不変条件: loading_ が true なら async_in_flight_ も true。
    // worker thread は async_gen_ (atomic) と async_result_ (mutex 保護) のみ触る。
    DocumentService& doc_service_;
    bool loading_ = false;
    bool async_in_flight_ = false;
    float loading_angle_ = 0.0f;
    std::pmr::wstring loading_path_;

    std::mutex async_mutex_;
    std::optional<AsyncLoadResult> async_result_;
    std::optional<FileLoadError> async_error_;
    std::atomic<uint32_t> async_gen_{ 0 };

    // 宣言順: preload_thread_ を preload_ctx_ より前に置く。reverse-destruction で
    // preload_ctx_ → preload_thread_ (join) → async_mutex_ ... の順に破壊され、
    // worker が async_mutex_ を触るのは join 完了より前に保証される。
    std::jthread preload_thread_;
    std::shared_ptr<PreloadContext> preload_ctx_;

    void ResetAsyncState();
};
