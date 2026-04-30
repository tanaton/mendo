#pragma once
#include "document_service.h"
#include "document.h"
#include "layout_cache.h"
#include "task_scheduler.h"
#include <string>
#include <optional>
#include <expected>
#include <mutex>
#include <atomic>

struct Theme;

// ワーカースレッドでのパース結果（Document + 推定高さ済み LayoutCache）
struct AsyncLoadResult {
    Document doc;
    LayoutCache cache;
};

// ファイル読み込みのオーケストレーションとローディングアニメーション状態を管理。
class FileLoadService {
public:
    explicit constexpr FileLoadService(DocumentService& doc_service) noexcept : doc_service_(doc_service) {}

    // ---- ローディングアニメーション状態 ----

    constexpr bool IsLoading() const noexcept { return loading_; }
    // スピナー非表示でも非同期パースが進行中なら true。ライブリロードのバースト時に
    // 重複スケジューリングを抑制するためのフラグ。
    constexpr bool IsAsyncLoading() const noexcept { return async_in_flight_; }
    constexpr float GetLoadingAngle() const noexcept { return loading_angle_; }

    void StartLoading(std::pmr::wstring path);
    void StopLoading() noexcept;
    void TickLoadingAnimation() noexcept;

    // ---- ファイル読み込み ----

    std::expected<void, FileLoadError> ExecuteLoad(Document& doc, LayoutCache& cache);

    // ---- 非同期ファイル読み込み ----

    void StartAsyncLoad(TaskScheduler& scheduler, HWND hwnd, UINT msg_id, const Theme& theme);
    std::optional<AsyncLoadResult> TakeAsyncResult();
    void CancelAsyncLoad() noexcept
    {
        async_gen_.fetch_add(1, std::memory_order_relaxed);
        async_in_flight_ = false;
    }

    // ---- パスアクセス ----

    std::wstring_view GetLoadingPath() const noexcept { return loading_path_; }
    void SetLoadingPath(std::pmr::wstring path) { loading_path_ = std::move(path); }

private:
    // 不変条件: loading_ が true なら async_in_flight_ も true。
    // worker thread は async_gen_ (atomic) と async_result_ (mutex 保護) のみ触る。
    DocumentService& doc_service_;
    bool loading_ = false;
    bool async_in_flight_ = false;
    float loading_angle_ = 0.0f;
    std::pmr::wstring loading_path_;

    // 非同期読み込み用
    std::mutex async_mutex_;
    std::optional<AsyncLoadResult> async_result_;
    std::atomic<uint32_t> async_gen_{ 0 };
};
