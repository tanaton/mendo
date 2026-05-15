#include <gtest/gtest.h>
#include "worker_latch.h"
#include <atomic>
#include <chrono>
#include <thread>
#include <vector>

TEST(WorkerLatch, WaitReturnsImmediatelyWhenIdle)
{
    WorkerLatch latch;
    const auto t0 = std::chrono::steady_clock::now();
    latch.Wait();
    const auto elapsed = std::chrono::steady_clock::now() - t0;
    EXPECT_LT(elapsed, std::chrono::milliseconds(50));
}

TEST(WorkerLatch, GuardReleaseAllowsWaitToReturn)
{
    WorkerLatch latch;
    {
        auto guard = latch.Acquire();
        // guard が live の間 Wait は block するため、テストでは別スコープで release する。
    }
    // ここでは in_flight = 0、Wait は即座に戻る。
    latch.Wait();
    SUCCEED();
}

TEST(WorkerLatch, MultipleGuardsTracked)
{
    WorkerLatch latch;
    auto g1 = latch.Acquire();
    auto g2 = latch.Acquire();
    auto g3 = latch.Acquire();
    {
        auto _ = std::move(g1);
    }
    std::atomic<bool> wait_returned{ false };
    std::thread waiter([&] {
        latch.Wait();
        wait_returned = true;
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    EXPECT_FALSE(wait_returned.load());

    {
        auto _ = std::move(g2);
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    EXPECT_FALSE(wait_returned.load());

    {
        auto _ = std::move(g3);
    }
    waiter.join();
    EXPECT_TRUE(wait_returned.load());
}

TEST(WorkerLatch, MoveAssignReleasesPreviousGuard)
{
    WorkerLatch latch;
    auto g1 = latch.Acquire();
    auto g2 = latch.Acquire();
    // g1 = std::move(g2) は g1 の参照を release してから g2 の参照を引き取る
    g1 = std::move(g2);
    // この時点で in_flight_ は 1
    std::atomic<bool> wait_returned{ false };
    std::thread waiter([&] {
        latch.Wait();
        wait_returned = true;
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    EXPECT_FALSE(wait_returned.load());
    {
        auto _ = std::move(g1);
    }
    waiter.join();
    EXPECT_TRUE(wait_returned.load());
}

TEST(WorkerLatch, WorkerThreadCompletionUnblocksWait)
{
    WorkerLatch latch;
    std::atomic<bool> worker_done{ false };
    std::thread worker([&, guard = latch.Acquire()]() mutable {
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
        worker_done = true;
        // guard は thread 終了時に destruct → release
    });
    latch.Wait();
    EXPECT_TRUE(worker_done.load());
    worker.join();
}

TEST(WorkerLatch, DefaultConstructedGuardIsNoop)
{
    WorkerLatch latch;
    {
        WorkerLatch::Guard g;
        // dtor が release を呼ばないこと (latch_ は nullptr)
    }
    latch.Wait();
    SUCCEED();
}

TEST(WorkerLatch, ConcurrentAcquireRelease)
{
    WorkerLatch latch;
    constexpr int N = 16;
    std::vector<std::thread> threads;
    std::atomic<int> done{ 0 };
    threads.reserve(N);
    for (int i = 0; i < N; ++i) {
        threads.emplace_back([&, guard = latch.Acquire()]() mutable {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            ++done;
        });
    }
    latch.Wait();
    EXPECT_EQ(done.load(), N);
    for (auto& t : threads) {
        t.join();
    }
}
