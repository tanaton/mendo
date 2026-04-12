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
// Win32非依存 — 完全にテスト可能。
class FileLoadService {
public:
    explicit constexpr FileLoadService(DocumentService& doc_service) noexcept : doc_service_(doc_service) {}

    // ---- ローディングアニメーション状態 ----

    constexpr bool IsLoading() const noexcept { return loading_; }
    constexpr float GetLoadingAngle() const noexcept { return loading_angle_; }

    // ファイルのローディングアニメーションを開始。
    void StartLoading(std::wstring_view path);

    // ローディングアニメーションを停止。
    void StopLoading() noexcept;

    // ローディングアニメーションを1フレーム進める。
    void TickLoadingAnimation() noexcept;

    // ---- ファイル読み込み ----

    // 保存されたローディングパスを使ってファイル読み込みを実行。
    // 成功時にdoc/cacheを更新しvoidを返す。失敗時にFileLoadErrorを返す。
    std::expected<void, FileLoadError> ExecuteLoad(Document& doc, LayoutCache& cache);

    // ---- 非同期ファイル読み込み ----

    // ワーカースレッドでファイル読み込み+パース+推定高さ計算を実行し、完了時にPostMessageで通知する。
    // theme は推定高さ計算用にコピーされる（ワーカースレッドでの安全なアクセスのため）。
    void StartAsyncLoad(TaskScheduler& scheduler, HWND hwnd, UINT msg_id, const Theme& theme);

    // ワーカースレッドで生成された結果を取り出す（UIスレッドから呼ぶ）。
    // 結果がない場合はnulloptを返す。
    std::optional<AsyncLoadResult> TakeAsyncResult();

    // 進行中の非同期読み込みをキャンセルする。
    void CancelAsyncLoad() noexcept { async_gen_.fetch_add(1, std::memory_order_relaxed); }

    // ---- パスアクセス ----

    std::wstring_view GetLoadingPath() const noexcept { return loading_path_; }
    void SetLoadingPath(std::wstring_view path) { loading_path_ = path; }

private:
    DocumentService& doc_service_;
    bool loading_ = false;
    float loading_angle_ = 0.0f;
    std::pmr::wstring loading_path_;

    // 非同期読み込み用
    std::mutex async_mutex_;
    std::optional<AsyncLoadResult> async_result_;
    std::atomic<uint32_t> async_gen_{ 0 };
};
