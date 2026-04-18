#include <gtest/gtest.h>
#include "task_scheduler.h"
#include <atomic>
#include <chrono>
#include <mutex>
#include <condition_variable>
#include <functional>
#include <set>
#include <thread>

namespace {

bool WaitFor(std::function<bool()> cond, int timeout_ms = 2000)
{
    const auto start = std::chrono::steady_clock::now();
    while (!cond()) {
        if (std::chrono::steady_clock::now() - start >
            std::chrono::milliseconds(timeout_ms)) {
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return true;
}

} // namespace

// ═══════════════════════════════════════════════
// 基本的な Post → 実行
// ═══════════════════════════════════════════════

TEST(TaskScheduler, SingleTaskExecutes)
{
    TaskScheduler sch;
    sch.Init(1);
    std::atomic<bool> ran{false};
    sch.Post([&] { ran.store(true); });
    EXPECT_TRUE(WaitFor([&] { return ran.load(); }));
    sch.Shutdown();
}

TEST(TaskScheduler, MultipleTasksAllExecute)
{
    TaskScheduler sch;
    sch.Init(2);
    constexpr int N = 100;
    std::atomic<int> count{0};
    for (int i = 0; i < N; ++i) {
        sch.Post([&] { count.fetch_add(1); });
    }
    EXPECT_TRUE(WaitFor([&] { return count.load() == N; }));
    sch.Shutdown();
    EXPECT_EQ(count.load(), N);
}

TEST(TaskScheduler, TasksRunOnWorkerThreadsNotCaller)
{
    TaskScheduler sch;
    sch.Init(2);
    const auto caller_id = std::this_thread::get_id();
    std::atomic<bool> ran{false};
    std::atomic<bool> different_thread{false};
    sch.Post([&] {
        different_thread.store(std::this_thread::get_id() != caller_id);
        ran.store(true);
    });
    EXPECT_TRUE(WaitFor([&] { return ran.load(); }));
    EXPECT_TRUE(different_thread.load());
    sch.Shutdown();
}

// ═══════════════════════════════════════════════
// 並行性
// ═══════════════════════════════════════════════

TEST(TaskScheduler, TasksDistributedAcrossWorkers)
{
    // 複数ワーカーで並行処理されることを観測する。
    // 各タスクがワーカーをブロックしている間に他のワーカーが別タスクを拾うはず。
    constexpr int WORKERS = 4;
    TaskScheduler sch;
    sch.Init(WORKERS);

    std::mutex m;
    std::set<std::thread::id> observed_threads;
    std::atomic<int> running{0};
    std::atomic<int> finished{0};

    for (int i = 0; i < WORKERS; ++i) {
        sch.Post([&] {
            running.fetch_add(1);
            {
                std::lock_guard lock(m);
                observed_threads.insert(std::this_thread::get_id());
            }
            // 他のワーカーも到達するまで少し待つ
            WaitFor([&] { return running.load() >= 2; }, 500);
            finished.fetch_add(1);
        });
    }

    EXPECT_TRUE(WaitFor([&] { return finished.load() == WORKERS; }));
    sch.Shutdown();

    // 最低でも2つ以上の異なるスレッドで処理されているはず
    std::lock_guard lock(m);
    EXPECT_GE(observed_threads.size(), 2u);
}

// ═══════════════════════════════════════════════
// Shutdown の挙動
// ═══════════════════════════════════════════════

TEST(TaskScheduler, ShutdownProcessesRemainingQueuedTasks)
{
    // ドキュメント: 「キューに残っているタスクをすべて処理してから終了」
    TaskScheduler sch;
    sch.Init(1);

    std::atomic<int> count{0};
    std::atomic<bool> first_entered{false};
    std::atomic<bool> gate_open{false};
    sch.Post([&] {
        first_entered.store(true);
        WaitFor([&] { return gate_open.load(); }, 2000);
        count.fetch_add(1);
    });
    // 先頭タスクがワーカーを掴んだことを確定させてから残りを投入する
    ASSERT_TRUE(WaitFor([&] { return first_entered.load(); }));
    for (int i = 0; i < 20; ++i) {
        sch.Post([&] { count.fetch_add(1); });
    }
    gate_open.store(true);

    sch.Shutdown();

    EXPECT_EQ(count.load(), 21);
}

TEST(TaskScheduler, ShutdownWithoutPostCompletes)
{
    TaskScheduler sch;
    sch.Init(4);
    sch.Shutdown(); // タスクがなくても正常終了する
    SUCCEED();
}

TEST(TaskScheduler, ShutdownTwiceIsSafe)
{
    TaskScheduler sch;
    sch.Init(2);
    std::atomic<int> count{0};
    sch.Post([&] { count.fetch_add(1); });
    EXPECT_TRUE(WaitFor([&] { return count.load() == 1; }));
    sch.Shutdown();
    sch.Shutdown(); // 2回目は no-op
    SUCCEED();
}

TEST(TaskScheduler, DestructorJoinsWorkersEvenIfShutdownNotCalled)
{
    std::atomic<int> count{0};
    {
        TaskScheduler sch;
        sch.Init(2);
        for (int i = 0; i < 10; ++i) {
            sch.Post([&] { count.fetch_add(1); });
        }
        // Shutdown を呼ばずスコープを抜ける → デストラクタで join される
    }
    EXPECT_EQ(count.load(), 10);
}

// ═══════════════════════════════════════════════
// タスクの型（move-only）
// ═══════════════════════════════════════════════

TEST(TaskScheduler, AcceptsMoveOnlyCallable)
{
    TaskScheduler sch;
    sch.Init(1);
    auto ptr = std::make_unique<std::atomic<int>>(0);
    auto* raw = ptr.get();
    sch.Post([p = std::move(ptr)]() mutable { p->fetch_add(42); });
    EXPECT_TRUE(WaitFor([&] { return raw->load() == 42; }));
    sch.Shutdown();
}

// ═══════════════════════════════════════════════
// 初期化なしで Post → Shutdown してもクラッシュしない
// ═══════════════════════════════════════════════

TEST(TaskScheduler, PostWithoutInitThenInitProcessesQueued)
{
    TaskScheduler sch;
    std::atomic<bool> ran{false};
    sch.Post([&] { ran.store(true); }); // Init 前に Post
    sch.Init(1); // ワーカー起動後にキュー消化
    EXPECT_TRUE(WaitFor([&] { return ran.load(); }));
    sch.Shutdown();
}

TEST(TaskScheduler, ZeroThreadsInitThenShutdownIsSafe)
{
    TaskScheduler sch;
    sch.Init(0); // ワーカーなし
    sch.Shutdown();
    SUCCEED();
}
