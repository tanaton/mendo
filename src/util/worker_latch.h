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
// Wait は worker が cv/mutex へのアクセスを完了するまで待機する。lock-free fast-path を
// 持つと「Wait が抜けた直後に latch 破棄 → worker が破棄済み cv/mutex を触る」破棄レースに
// なるため、判定は常に mutex 下で行う。
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
            // デクリメントと notify を同一 mutex 区間で完結させる。これにより Wait が
            // 抜けた時点で worker は cv/mutex へのアクセスを終えており、直後に latch が
            // 破棄されても use-after-free にならない。acq_rel は worker が触った sink
            // メモリの可視性を Wait 側へ保証する。
            {
                const std::lock_guard lock(latch_->mutex_);
                if (latch_->in_flight_.fetch_sub(1, std::memory_order_acq_rel) == 1) {
                    latch_->cv_.notify_all();
                }
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
        std::unique_lock lock(mutex_);
        cv_.wait(lock, [this] {
            return in_flight_.load(std::memory_order_relaxed) == 0;
        });
    }

private:
    std::atomic<int> in_flight_{ 0 };
    std::mutex mutex_;
    std::condition_variable cv_;
};
