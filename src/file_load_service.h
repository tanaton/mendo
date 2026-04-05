#pragma once
#include "document_service.h"
#include "document.h"
#include "layout_cache.h"
#include "task_scheduler.h"
#include <string>
#include <optional>
#include <mutex>
#include <atomic>

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
    // 成功時にtrueを返す。呼び出し元はdocからディレクトリ/ファイルパスを参照すること。
    bool ExecuteLoad(Document& doc, LayoutCache& cache);

    // ---- 非同期ファイル読み込み ----

    // ワーカースレッドでファイル読み込み+パースを実行し、完了時にPostMessageで通知する。
    void StartAsyncLoad(TaskScheduler& scheduler, HWND hwnd, UINT msg_id);

    // ワーカースレッドで生成されたDocumentを取り出す（UIスレッドから呼ぶ）。
    // 結果がない場合はnulloptを返す。
    std::optional<Document> TakeAsyncResult();

    // 進行中の非同期読み込みをキャンセルする。
    void CancelAsyncLoad() noexcept { async_gen_.fetch_add(1, std::memory_order_relaxed); }

    // ---- パスアクセス ----

    constexpr std::wstring_view GetLoadingPath() const noexcept { return loading_path_; }
    constexpr void SetLoadingPath(std::wstring_view path) { loading_path_ = path; }

private:
    DocumentService& doc_service_;
    bool loading_ = false;
    float loading_angle_ = 0.0f;
    std::pmr::wstring loading_path_;

    // 非同期読み込み用
    std::mutex async_mutex_;
    std::optional<Document> async_result_;
    std::atomic<uint32_t> async_gen_{ 0 };
};
