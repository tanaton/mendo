#include <gtest/gtest.h>
#include "worker_latch.h"
#include <atomic>
#include <chrono>
#include <thread>
#include <vector>

// 生きている guard が無ければ Wait はすぐ戻る。既定構築の guard は latch を持たないので破棄しても何もしない。
TEST(WorkerLatch, WaitReturnsWithoutLiveGuard)
{
    WorkerLatch latch;
    latch.Wait();
    {
        auto guard = latch.Acquire();
    }
    latch.Wait();
    {
        WorkerLatch::Guard g;
    }
    latch.Wait();
}

// 最後の guard が外れるまで Wait は戻らない。
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

// 空のときの 1 件目だけ投函させ、期限切れでも 1 件は進めて残りは順序を保って次に回す。
TEST(PostedInbox, PostsOnceAndKeepsOrderOfLeftovers)
{
    PostedInbox<int> inbox;
    EXPECT_TRUE(inbox.Push(1));
    EXPECT_FALSE(inbox.Push(2));
    EXPECT_FALSE(inbox.Push(3));

    std::vector<int> seen;
    const auto expired = std::chrono::steady_clock::time_point::min();
    inbox.Drain(expired, [&](int v) { seen.push_back(v); });
    EXPECT_EQ(seen.size(), 1u);
    EXPECT_TRUE(inbox.HasLeftover());
    EXPECT_FALSE(inbox.Push(4)) << "未回収が残っている間は投函しない";

    inbox.Drain(std::chrono::steady_clock::time_point::max(), [&](int v) { seen.push_back(v); });
    EXPECT_FALSE(inbox.HasLeftover());
    EXPECT_EQ(seen, (std::vector<int>{ 1, 2, 3, 4 }));
    EXPECT_TRUE(inbox.Push(5));
    EXPECT_EQ(inbox.TakeAll().size(), 1u);
}
