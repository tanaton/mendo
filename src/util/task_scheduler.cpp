#include "task_scheduler.h"
#include "profiler.h"
#include <windows.h>
#include <objbase.h>
#include <algorithm>
#include <cstdint>

namespace {

// 初期化に失敗したワーカーは NotWorker に戻し、再試行も CoUninitialize もしない。
enum class WorkerCom : uint8_t {
    NotWorker,
    Uninitialized,
    Initialized,
};

thread_local WorkerCom t_worker_com = WorkerCom::NotWorker;

} // namespace

TaskScheduler::~TaskScheduler()
{
    Shutdown();
}

void TaskScheduler::Init(int thread_count)
{
    const std::lock_guard lock(mutex_);
    // 二重 Init で worker が累積しないよう、最初の指定を維持する。
    if (thread_count_ != 0) {
        return;
    }
    shutdown_ = false;
    thread_count_ = static_cast<size_t>(std::max(thread_count, 0));
    // Init 前に積まれたタスクを消化する。
    if (!queue_.empty()) {
        StartWorkersLocked();
    }
}

void TaskScheduler::StartWorkersLocked()
{
    if (!workers_.empty()) {
        return;
    }
    workers_.reserve(thread_count_);
    for (size_t i = 0; i < thread_count_; ++i) {
        workers_.emplace_back(&TaskScheduler::WorkerLoop, this);
    }
}

bool TaskScheduler::Post(std::move_only_function<void()> task)
{
    {
        const std::lock_guard lock(mutex_);
        // Shutdown 後の Post は棄却。判定を lock 外で行うと、Shutdown の store → join 完了の後に
        // push して true を返し、誰も実行しないタスクが残る。
        if (shutdown_) {
            return false;
        }
        if (queue_.size() >= MAX_PENDING_TASKS) {
            MENDO_TRACE("TaskScheduler: queue saturated, task dropped");
            return false;
        }
        queue_.push(std::move(task));
        StartWorkersLocked();
    }
    cv_.notify_one();
    return true;
}

void TaskScheduler::Shutdown()
{
    // store は mutex 下で行う。lock 外だと、worker が述語 (false) を評価してから wait に入るまでの
    // 隙間に store と notify_all が割り込んで通知を取りこぼし、join が永久に戻らない。
    // store 後の Post は workers_ に触れずに棄却されるので、以降は lock 外で join してよい。
    {
        const std::lock_guard lock(mutex_);
        shutdown_ = true;
    }
    cv_.notify_all();
    for (auto& t : workers_) {
        if (t.joinable()) {
            t.join();
        }
    }
    workers_.clear();
    thread_count_ = 0;
}

void TaskScheduler::EnsureWorkerCom() noexcept
{
    if (t_worker_com != WorkerCom::Uninitialized) {
        return;
    }
    t_worker_com = SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED)) ? WorkerCom::Initialized : WorkerCom::NotWorker;
}

void TaskScheduler::WorkerLoop()
{
    t_worker_com = WorkerCom::Uninitialized;

    while (true) {
        std::move_only_function<void()> task;
        {
            std::unique_lock lock(mutex_);
            cv_.wait(lock, [this] {
                return !queue_.empty() || shutdown_;
            });
            // predicate から戻った時点で queue に残りがあれば shutdown 中でも処理する
            if (queue_.empty()) {
                break;
            }
            task = std::move(queue_.front());
            queue_.pop();
        }
        task();
    }

    if (t_worker_com == WorkerCom::Initialized) {
        CoUninitialize();
    }
}
