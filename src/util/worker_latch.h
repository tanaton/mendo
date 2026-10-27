#pragma once
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <iterator>
#include <mutex>
#include <utility>
#include <vector>

// worker の結果を UI スレッドへ渡す受け箱。空のときに入った 1 件目だけ受け手へ投函させ、まとめて回収させる。
// 1 件ずつ投函すると、投函メッセージは描画・入力より先に取り出されるので、時間で区切った回収が間を空けずに続いてしまう。
template <class T>
class PostedInbox {
public:
    // worker から呼ぶ。true なら投函する (それ以外は通知済みか、空き時間の回収が拾う)。
    bool Push(T item)
    {
        const std::lock_guard lock(mutex_);
        items_.push_back(std::move(item));
        return items_.size() == 1;
    }

    // UI スレッドから呼ぶ。先頭から deadline まで apply し、処理した分を返す。必ず進むよう 1 件目は期限を見ない。
    // 残りは先頭へ戻し、HasLeftover が真になる。
    template <class Apply>
    std::vector<T> Drain(std::chrono::steady_clock::time_point deadline, Apply&& apply)
    {
        std::vector<T> items;
        {
            const std::lock_guard lock(mutex_);
            items.swap(items_);
        }
        size_t done = 0;
        for (; done < items.size(); ++done) {
            if (done > 0 && std::chrono::steady_clock::now() >= deadline) {
                break;
            }
            apply(items[done]);
        }
        leftover_ = done < items.size();
        if (leftover_) {
            const auto rest = items.begin() + static_cast<std::ptrdiff_t>(done);
            const std::lock_guard lock(mutex_);
            items_.insert(items_.begin(), std::make_move_iterator(rest), std::make_move_iterator(items.end()));
            items.erase(rest, items.end());
        }
        return items;
    }

    // 未回収の結果を捨てる。解放は lock の外 (戻り値の破棄) で行う。UI スレッド専用。
    std::vector<T> TakeAll()
    {
        std::vector<T> items;
        {
            const std::lock_guard lock(mutex_);
            items.swap(items_);
        }
        leftover_ = false;
        return items;
    }

    // 新しい結果は投函で知らせるので、続きが要るのは時間切れで残したときだけ。UI スレッド専用。
    bool HasLeftover() const noexcept
    {
        return leftover_;
    }

private:
    std::mutex mutex_;
    std::vector<T> items_;
    bool leftover_ = false;
};

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
        Guard(const Guard&) = delete;
        Guard& operator=(const Guard&) = delete;

        ~Guard()
        {
            if (!latch_) {
                return;
            }
            // デクリメントと notify を同一 mutex 区間で完結させる。これにより Wait が
            // 抜けた時点で worker は cv/mutex へのアクセスを終えており、直後に latch が
            // 破棄されても use-after-free にならない。
            const std::lock_guard lock(latch_->mutex_);
            if (--latch_->in_flight_ == 0) {
                latch_->cv_.notify_all();
            }
        }

    private:
        WorkerLatch* latch_ = nullptr;
    };

    WorkerLatch() = default;
    WorkerLatch(const WorkerLatch&) = delete;
    WorkerLatch& operator=(const WorkerLatch&) = delete;

    [[nodiscard]] Guard Acquire() noexcept
    {
        const std::lock_guard lock(mutex_);
        ++in_flight_;
        return Guard{ this };
    }

    void Wait() noexcept
    {
        std::unique_lock lock(mutex_);
        cv_.wait(lock, [this] {
            return in_flight_ == 0;
        });
    }

private:
    int in_flight_ = 0;
    std::mutex mutex_;
    std::condition_variable cv_;
};
