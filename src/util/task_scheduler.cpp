#include "task_scheduler.h"
#include <windows.h>
#include <objbase.h>

TaskScheduler::~TaskScheduler()
{
    Shutdown();
}

void TaskScheduler::Init(int thread_count)
{
    shutdown_.store(false);
    workers_.reserve(thread_count);
    for (int i = 0; i < thread_count; ++i) {
        workers_.emplace_back(&TaskScheduler::WorkerLoop, this);
    }
}

bool TaskScheduler::Post(std::move_only_function<void()> task)
{
    // Shutdown 後の Post は早期棄却。worker が join 中の状態で queue に積んでも実行されない。
    if (shutdown_.load(std::memory_order_acquire)) {
        return false;
    }
    {
        const std::lock_guard lock(mutex_);
        if (queue_.size() >= MAX_PENDING_TASKS) {
            OutputDebugStringW(L"[TaskScheduler] queue saturated, dropping task\n");
            return false;
        }
        queue_.push(std::move(task));
    }
    cv_.notify_one();
    return true;
}

void TaskScheduler::Shutdown()
{
    // shutdown_ は atomic なので lock を取らずに store して良い。cv_.wait(lock, predicate) は
    // unlock と wait を atomic に行うため、lock 外 store でも notify_all を取り逃さない。
    shutdown_.store(true, std::memory_order_release);
    cv_.notify_all();
    for (auto& t : workers_) {
        if (t.joinable()) {
            t.join();
        }
    }
    workers_.clear();
}

void TaskScheduler::WorkerLoop()
{
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);

    while (true) {
        std::move_only_function<void()> task;
        {
            std::unique_lock lock(mutex_);
            cv_.wait(lock, [this] {
                return !queue_.empty() || shutdown_.load();
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

    CoUninitialize();
}
