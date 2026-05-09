#pragma once
#include <atomic>
#include <condition_variable>
#include <mutex>

// 共有 TaskScheduler に Post した worker のライフタイムを所有者が join できるようにする latch。
// 利用パターン:
//   auto guard = latch_.Acquire();        // Post 直前 (UI スレッド)
//   const bool posted = scheduler.Post([this, g = std::move(guard)] { /* worker body */ });
//   if (!posted) { /* g が destruct されて自動的に Release */ }
//   ...
//   ~Owner() { latch_.Wait(); }            // 既に Post 済みの worker 完了を待つ
//
// Wait は in_flight が 0 の場合 lock を取らず即時 return する。
class WorkerLatch {
public:
    class Guard {
    public:
        Guard() noexcept = default;
        explicit Guard(WorkerLatch* latch) noexcept : latch_(latch)
        {}
        Guard(Guard&& other) noexcept : latch_(other.latch_)
        {
            other.latch_ = nullptr;
        }
        Guard& operator=(Guard&& other) noexcept
        {
            if (this != &other) {
                ReleaseIfActive();
                latch_ = other.latch_;
                other.latch_ = nullptr;
            }
            return *this;
        }
        Guard(const Guard&) = delete;
        Guard& operator=(const Guard&) = delete;

        ~Guard()
        {
            ReleaseIfActive();
        }

    private:
        void ReleaseIfActive() noexcept
        {
            if (!latch_) {
                return;
            }
            // fetch_sub の acq_rel は Wait 側 acquire load と対になり、worker が触った
            // sink メモリの可視性を保証する。
            if (latch_->in_flight_.fetch_sub(1, std::memory_order_acq_rel) == 1) {
                const std::lock_guard lock(latch_->mutex_);
                latch_->cv_.notify_all();
            }
            latch_ = nullptr;
        }

        WorkerLatch* latch_ = nullptr;
    };

    WorkerLatch() = default;
    WorkerLatch(const WorkerLatch&) = delete;
    WorkerLatch& operator=(const WorkerLatch&) = delete;

    [[nodiscard]] Guard Acquire() noexcept
    {
        // Post 自身が release バリアを供給するため、relaxed で十分。
        in_flight_.fetch_add(1, std::memory_order_relaxed);
        return Guard{ this };
    }

    void Wait() noexcept
    {
        if (in_flight_.load(std::memory_order_acquire) == 0) {
            return;
        }
        std::unique_lock lock(mutex_);
        cv_.wait(lock, [this] {
            return in_flight_.load(std::memory_order_acquire) == 0;
        });
    }

private:
    std::atomic<int> in_flight_{ 0 };
    std::mutex mutex_;
    std::condition_variable cv_;
};
