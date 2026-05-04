#pragma once
#include <vector>
#include <queue>
#include <deque>
#include <memory_resource>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <functional>

// 汎用タスクスケジューラ。
// 複数のワーカースレッドでタスクを並行実行するスレッドプール。
// 各ワーカースレッドではCOMが初期化される（COINIT_MULTITHREADED）。
// Post() はスレッドセーフ。Init() / Shutdown() はUIスレッドから呼び出す。
class TaskScheduler {
public:
    // 同時にキューに保留できるタスク数の上限。
    // Why: 異常系（巨大ドキュメントの大量画像/Mermaid 要求）でキューが青天井に
    // 膨らむのを防ぎ、move_only_function キャプチャによるヒープ消費を制限する。
    static constexpr size_t MAX_PENDING_TASKS = 1024;

    TaskScheduler() = default;
    ~TaskScheduler();

    TaskScheduler(const TaskScheduler&) = delete;
    TaskScheduler& operator=(const TaskScheduler&) = delete;

    void Init(int thread_count);

    // タスクを投入する。キューが MAX_PENDING_TASKS に達している場合は
    // タスクを破棄して false を返し、デバッグ出力に警告を残す。
    // 呼び出し側は戻り値を無視してもよい（破棄されたタスクは単に未実行）。
    bool Post(std::move_only_function<void()> task);

    void Shutdown();

    // 起動済みワーカー数。Init 前 / Shutdown 後は 0。chunk size などの動的調整に使う。
    size_t WorkerCount() const noexcept { return workers_.size(); }

private:
    void WorkerLoop();

    std::vector<std::thread> workers_;
    std::queue<std::move_only_function<void()>, std::pmr::deque<std::move_only_function<void()>>> queue_;
    std::mutex mutex_;
    std::condition_variable cv_;
    std::atomic<bool> shutdown_{ false };
};
