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

void TaskScheduler::Post(std::function<void()> task)
{
    {
        const std::lock_guard lock(mutex_);
        queue_.push(std::move(task));
    }
    cv_.notify_one();
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
        std::function<void()> task;
        {
            std::unique_lock lock(mutex_);
            cv_.wait(lock, [this] {
                return !queue_.empty() || shutdown_.load();
            });
            if (queue_.empty()) {
                if (shutdown_.load()) {
                    break;
                }
                continue;
            }
            task = std::move(queue_.front());
            queue_.pop();
        }
        task();
    }

    CoUninitialize();
}
