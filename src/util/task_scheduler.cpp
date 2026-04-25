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
    {
        const std::lock_guard lock(mutex_);
        shutdown_.store(true);
    }
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
