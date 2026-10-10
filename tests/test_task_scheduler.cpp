#include <gtest/gtest.h>
#include "task_scheduler.h"
#include "test_helpers.h"
#include <atomic>
#include <chrono>
#include <mutex>
#include <condition_variable>
#include <future>
#include <memory>
#include <vector>
#include <set>
#include <thread>

// ═══════════════════════════════════════════════
// 基本的な Post → 実行
// ═══════════════════════════════════════════════

TEST(TaskScheduler, SingleTaskExecutes)
{
    TaskScheduler sch;
    sch.Init(1);
    std::atomic<bool> ran{false};
    sch.Post([&] { ran.store(true); });
    EXPECT_TRUE(PollUntil([&] { return ran.load(); }));
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
    EXPECT_TRUE(PollUntil([&] { return count.load() == N; }));
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
    EXPECT_TRUE(PollUntil([&] { return ran.load(); }));
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
            PollUntil([&] { return running.load() >= 2; }, std::chrono::milliseconds(500));
            finished.fetch_add(1);
        });
    }

    EXPECT_TRUE(PollUntil([&] { return finished.load() == WORKERS; }));
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
        PollUntil([&] { return gate_open.load(); }, std::chrono::seconds(2));
        count.fetch_add(1);
    });
    // 先頭タスクがワーカーを掴んだことを確定させてから残りを投入する
    ASSERT_TRUE(PollUntil([&] { return first_entered.load(); }));
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
    EXPECT_TRUE(PollUntil([&] { return count.load() == 1; }));
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
// Shutdown の競合 (回帰)
// ═══════════════════════════════════════════════

namespace {

// ハングしてもスイート全体を止めないよう、本体を切り離したスレッドで走らせて期限で見切る。
// 期限切れ時のスレッドは detach のまま残す (ハングしたスケジューラはそのスレッドのスタック上にある)。
template <class Body>
::testing::AssertionResult CompletesWithin(std::chrono::milliseconds budget, Body body)
{
    auto progress = std::make_shared<std::atomic<int>>(0);
    auto done = std::make_shared<std::promise<void>>();
    auto finished = done->get_future();
    std::thread([progress, done, body = std::move(body)]() mutable {
        body(*progress);
        done->set_value();
    }).detach();
    if (finished.wait_for(budget) == std::future_status::ready) {
        return ::testing::AssertionSuccess();
    }
    return ::testing::AssertionFailure() << "did not complete within " << budget.count() << "ms (stuck at iteration " << progress->load() << ")";
}

constexpr auto kHangBudget = std::chrono::seconds(10);
// ハングの隙間は数命令幅で検出が確率的なため長めに回す。
constexpr auto kShutdownRaceLoopTime = std::chrono::milliseconds(100);
// Post と Shutdown の競合は数ループで踏めるので短くてよい。
constexpr auto kPostRaceLoopTime = std::chrono::milliseconds(30);

} // namespace

// 868f19d: shutdown_ を lock 外で store すると、worker が述語を評価してから wait に入るまでの
// 隙間に store と notify_all が割り込み、通知を取りこぼして join が永久に戻らなかった。
// Init 直後 (worker が初回の述語評価に向かう最中) に Shutdown を繰り返して隙間を突く。
// 隙間は数命令幅なので検出は確率的 (修正前実装で数千回に 1 回程度ハング)。
TEST(TaskScheduler, ShutdownRightAfterInitNeverHangs)
{
    EXPECT_TRUE(CompletesWithin(kHangBudget, [](std::atomic<int>& iteration) {
        const auto deadline = std::chrono::steady_clock::now() + kShutdownRaceLoopTime;
        while (std::chrono::steady_clock::now() < deadline) {
            TaskScheduler sch;
            sch.Init(4);
            sch.Shutdown();
            iteration.fetch_add(1);
        }
    }));
}

// Post の契約: true を返したタスクは必ず実行され、false のタスクは実行されない。
// shutdown 判定を lock 外で行うと、Shutdown の join 完了後に push されて true を返し、
// 誰も実行しないタスクが残る。
TEST(TaskScheduler, PostRacingShutdownRunsExactlyAcceptedTasks)
{
    EXPECT_TRUE(CompletesWithin(kHangBudget, [](std::atomic<int>& iteration) {
        constexpr int kPosters = 3;
        const auto deadline = std::chrono::steady_clock::now() + kPostRaceLoopTime;
        while (std::chrono::steady_clock::now() < deadline) {
            std::atomic<int> accepted{ 0 };
            std::atomic<int> executed{ 0 };
            std::atomic<int> accepted_after_shutdown{ 0 };
            std::atomic<bool> shutdown_returned{ false };
            std::atomic<int> started{ 0 };
            {
                TaskScheduler sch;
                sch.Init(2);
                std::vector<std::thread> posters;
                for (int p = 0; p < kPosters; ++p) {
                    posters.emplace_back([&] {
                        started.fetch_add(1);
                        while (true) {
                            const bool after = shutdown_returned.load();
                            if (sch.Post([&] { executed.fetch_add(1); })) {
                                accepted.fetch_add(1);
                                if (after) {
                                    accepted_after_shutdown.fetch_add(1);
                                }
                            }
                            if (after) {
                                break;
                            }
                        }
                    });
                }
                while (started.load() < kPosters) {
                    std::this_thread::yield();
                }
                sch.Shutdown();
                shutdown_returned.store(true);
                for (auto& t : posters) {
                    t.join();
                }
                // Shutdown は worker を join 済みなので、受理済みタスクはこの時点で全て実行されているはず。
                if (accepted_after_shutdown.load() != 0 || executed.load() != accepted.load()) {
                    ADD_FAILURE() << "iteration=" << iteration.load() << " accepted=" << accepted.load()
                                  << " executed=" << executed.load() << " accepted_after_shutdown=" << accepted_after_shutdown.load();
                    return;
                }
            }
            iteration.fetch_add(1);
        }
    }));
}

// ═══════════════════════════════════════════════
// タスクの型（move-only）
// ═══════════════════════════════════════════════

TEST(TaskScheduler, AcceptsMoveOnlyCallable)
{
    TaskScheduler sch;
    sch.Init(1);
    // 実行後のタスクは worker 側で破棄されるので、結果はタスク外の変数で受け取る。
    std::atomic<int> result{ 0 };
    auto ptr = std::make_unique<int>(42);
    sch.Post([p = std::move(ptr), &result]() mutable { result.store(*p); });
    EXPECT_TRUE(PollUntil([&] { return result.load() == 42; }));
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
    EXPECT_TRUE(PollUntil([&] { return ran.load(); }));
    sch.Shutdown();
}

// ワーカーは遅延起動だが、ParallelFor の分割数は起動前から確定している必要がある。
TEST(TaskScheduler, WorkerCountReportsConfiguredCountBeforeFirstPost)
{
    TaskScheduler sch;
    EXPECT_EQ(sch.WorkerCount(), 0u);
    sch.Init(3);
    EXPECT_EQ(sch.WorkerCount(), 3u);
    sch.Shutdown();
    EXPECT_EQ(sch.WorkerCount(), 0u);
}

TEST(TaskScheduler, ReinitAfterShutdownRunsTasks)
{
    TaskScheduler sch;
    sch.Init(1);
    sch.Shutdown();
    sch.Init(2);
    std::atomic<bool> ran{ false };
    EXPECT_TRUE(sch.Post([&] { ran.store(true); }));
    EXPECT_TRUE(PollUntil([&] { return ran.load(); }));
    sch.Shutdown();
}

TEST(TaskScheduler, ZeroThreadsInitThenShutdownIsSafe)
{
    TaskScheduler sch;
    sch.Init(0); // ワーカーなし
    sch.Shutdown();
    SUCCEED();
}
