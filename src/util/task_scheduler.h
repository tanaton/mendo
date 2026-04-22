#pragma once
#include <vector>
#include <queue>
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
    TaskScheduler() = default;
    ~TaskScheduler();

    TaskScheduler(const TaskScheduler&) = delete;
    TaskScheduler& operator=(const TaskScheduler&) = delete;

    void Init(int thread_count);
    void Post(std::move_only_function<void()> task);
    void Shutdown();

private:
    void WorkerLoop();

    std::vector<std::thread> workers_;
    std::queue<std::move_only_function<void()>> queue_;
    std::mutex mutex_;
    std::condition_variable cv_;
    std::atomic<bool> shutdown_{ false };
};
