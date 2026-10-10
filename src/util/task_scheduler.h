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

// 各ワーカースレッドではCOMが初期化される（COINIT_MULTITHREADED）。
// Post() はスレッドセーフ。Init() / Shutdown() はUIスレッドから呼び出す。
// ワーカーは初回 Post まで起動しない。小さい文書だけを開くセッションでスレッドを作らずに済む。
class TaskScheduler {
public:
    // Why: 異常系（巨大ドキュメントの大量画像/Mermaid 要求）でキューが青天井に
    // 膨らむのを防ぎ、move_only_function キャプチャによるヒープ消費を制限する。
    static constexpr size_t MAX_PENDING_TASKS = 1024;

    TaskScheduler() = default;
    ~TaskScheduler();

    TaskScheduler(const TaskScheduler&) = delete;
    TaskScheduler& operator=(const TaskScheduler&) = delete;

    void Init(int thread_count);

    // 呼び出し側は戻り値を無視してもよい（破棄されたタスクは単に未実行）。
    bool Post(std::move_only_function<void()> task);

    void Shutdown();

    // 起動前でも Init で指定した数を返す (ParallelFor の分割数に使うため)。Init 前 / Shutdown 後は 0。
    size_t WorkerCount() const noexcept
    {
        return thread_count_;
    }

private:
    // mutex_ を保持して呼ぶ。
    void StartWorkersLocked();
    void WorkerLoop();

    size_t thread_count_ = 0;
    std::vector<std::thread> workers_;
    std::queue<std::move_only_function<void()>, std::pmr::deque<std::move_only_function<void()>>> queue_;
    std::mutex mutex_;
    std::condition_variable cv_;
    std::atomic<bool> shutdown_{ false };
};
