#include "task_scheduler.h"
#include <windows.h>
#include <objbase.h>
#include <algorithm>
#include <cstdint>
#include <utility>

namespace {

// 初期化に失敗したワーカーは NotWorker に戻し、再試行も CoUninitialize もしない。
enum class WorkerCom : uint8_t {
    NotWorker,
    Uninitialized,
    Initialized,
};

thread_local WorkerCom t_worker_com = WorkerCom::NotWorker;

} // namespace

TaskScheduler::TaskScheduler(int thread_count, bool below_normal) noexcept
    : thread_count_(static_cast<size_t>(std::max(thread_count, 0))), below_normal_(below_normal)
{
}

TaskScheduler::~TaskScheduler()
{
    Shutdown();
}

void TaskScheduler::StartWorkersLocked()
{
    // 待ちのタスクが、仕事をしていないワーカー (待っているか起動中) より多いときだけ足す。一度に全本数を作らないので、
    // 畳んだ後のばらばらの Post や小さな ParallelFor でスレッドを作り過ぎない。
    const size_t count = thread_count_.load(std::memory_order_relaxed);
    while (workers_.size() < count && queue_.size() > workers_.size() - busy_) {
        workers_.emplace_back(&TaskScheduler::WorkerLoop, this, generation_);
    }
}

bool TaskScheduler::PostImpl(std::move_only_function<void()> task, bool front)
{
    {
        const std::lock_guard lock(mutex_);
        // Shutdown 後の Post は棄却。判定を lock 外で行うと、Shutdown の store → join 完了の後に
        // push して true を返し、誰も実行しないタスクが残る。
        if (shutdown_) {
            return false;
        }
        if (front) {
            queue_.push_front(std::move(task));
        }
        else {
            queue_.push_back(std::move(task));
        }
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
    thread_count_.store(0, std::memory_order_relaxed);
}

bool TaskScheduler::ReleaseIdleWorkers()
{
    std::vector<std::thread> retired;
    {
        const std::lock_guard lock(mutex_);
        if (shutdown_ || workers_.empty() || !queue_.empty() || busy_ != 0) {
            return false;
        }
        ++generation_;
        retired = std::exchange(workers_, {});
    }
    // 待っているだけなので、起こせばすぐ終わる。この間の Post は新しい世代のワーカーを作って任せる。
    cv_.notify_all();
    for (auto& t : retired) {
        t.join();
    }
    return true;
}

void TaskScheduler::EnsureWorkerCom() noexcept
{
    if (t_worker_com != WorkerCom::Uninitialized) {
        return;
    }
    t_worker_com = SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED)) ? WorkerCom::Initialized : WorkerCom::NotWorker;
}

void TaskScheduler::WorkerLoop(uint64_t generation)
{
    t_worker_com = WorkerCom::Uninitialized;
    if (below_normal_) {
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
    }

    bool ran = false;
    while (true) {
        std::move_only_function<void()> task;
        {
            std::unique_lock lock(mutex_);
            // 前の周回で動かしたタスクの分を、取り直したロックの下で戻す (ロックを増やさない)。
            if (std::exchange(ran, false)) {
                --busy_;
            }
            cv_.wait(lock, [this, generation] {
                return !queue_.empty() || shutdown_ || generation != generation_;
            });
            // predicate から戻った時点で queue に残りがあれば shutdown 中でも処理する。畳まれたワーカーは残りを新しい世代に任せる。
            if (queue_.empty() || generation != generation_) {
                break;
            }
            task = std::move(queue_.front());
            queue_.pop_front();
            ++busy_;
            ran = true;
        }
        task();
    }

    if (t_worker_com == WorkerCom::Initialized) {
        CoUninitialize();
    }
}
