#include <gtest/gtest.h>
#include "async_load_coordinator.h"
#include "task_scheduler.h"
#include "test_helpers.h"
#include "theme.h"

namespace {

// scheduler は fixture で 1 度だけ Init/Shutdown する。各テストの Init/Shutdown は
// ワーカー thread の生成・join を毎回招きトータル数百ms に達するため。
class AsyncLoadCoordinatorTest : public ::testing::Test {
protected:
    static TaskScheduler scheduler_;

    static void SetUpTestSuite()
    {
        scheduler_.Init(1);
    }
    static void TearDownTestSuite()
    {
        scheduler_.Shutdown();
    }
};

TaskScheduler AsyncLoadCoordinatorTest::scheduler_;

constexpr auto kPollTimeout = std::chrono::seconds(5);

} // namespace

TEST_F(AsyncLoadCoordinatorTest, DefaultConstructionIsInactive)
{
    AsyncLoadCoordinator c;
    EXPECT_FALSE(c.IsActive());
    EXPECT_FALSE(c.TakeResult().has_value());
    EXPECT_FALSE(c.TakeError().has_value());
}

TEST_F(AsyncLoadCoordinatorTest, CancelOnIdleIsSafe)
{
    AsyncLoadCoordinator c;
    c.Cancel();
    EXPECT_FALSE(c.IsActive());
    EXPECT_FALSE(c.TakeResult().has_value());
    EXPECT_FALSE(c.TakeError().has_value());
}

TEST_F(AsyncLoadCoordinatorTest, ShutdownSchedulerCausesReadFailedError)
{
    // Shutdown 済み scheduler への Post は false を返し、coordinator は同期的に
    // error_=ReadFailed を立てて in_flight_=false に戻す経路を踏む。
    TaskScheduler local;
    local.Init(1);
    local.Shutdown();

    AsyncLoadCoordinator c;
    c.Start(local, std::pmr::wstring(L"unused.md"), nullptr, 0, GetLightTheme());

    EXPECT_FALSE(c.IsActive());
    auto err = c.TakeError();
    ASSERT_TRUE(err.has_value());
    EXPECT_EQ(*err, FileLoadError::ReadFailed);
    EXPECT_FALSE(c.TakeError().has_value());
}

TEST_F(AsyncLoadCoordinatorTest, SuccessfulLoadProducesResult)
{
    TempFile tmp(L"aload_ok", "# Title\n\nbody text\n");

    AsyncLoadCoordinator c;
    c.Start(scheduler_, tmp.PmrPath(), nullptr, 0, GetLightTheme());

    std::optional<AsyncLoadResult> result;
    PollUntil([&] { result = c.TakeResult(); return result.has_value(); }, kPollTimeout);
    ASSERT_TRUE(result.has_value());
    EXPECT_TRUE(result->heights_estimated);
    EXPECT_FALSE(result->doc.IsEmpty());
    EXPECT_FALSE(c.IsActive());
}

TEST_F(AsyncLoadCoordinatorTest, NotFoundFileProducesError)
{
    AsyncLoadCoordinator c;
    c.Start(scheduler_, std::pmr::wstring(L"C:\\__mendo_no_such_file__.md"), nullptr, 0, GetLightTheme());

    std::optional<FileLoadError> err;
    PollUntil([&] { err = c.TakeError(); return err.has_value(); }, kPollTimeout);
    ASSERT_TRUE(err.has_value());
    EXPECT_EQ(*err, FileLoadError::NotFound);
}

TEST_F(AsyncLoadCoordinatorTest, CancelBeforeWorkerCompletesDiscardsResult)
{
    TempFile tmp(L"aload_cancel", "# heading\n\nparagraph\n");

    AsyncLoadCoordinator c;
    c.Start(scheduler_, tmp.PmrPath(), nullptr, 0, GetLightTheme());
    c.Cancel();

    EXPECT_FALSE(c.IsActive());
    // worker が完了しても sink に publish されないことを確認 (gen mismatch で弾かれる)。
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    EXPECT_FALSE(c.TakeResult().has_value());
    EXPECT_FALSE(c.TakeError().has_value());
}

TEST_F(AsyncLoadCoordinatorTest, DestructorWaitsForRunningWorker)
{
    // dtor が走行中 worker の latch を待たないと共有 scheduler 経由で UAF。
    TempFile tmp(L"aload_dtor", "just text\n");
    {
        AsyncLoadCoordinator c;
        c.Start(scheduler_, tmp.PmrPath(), nullptr, 0, GetLightTheme());
    }
    SUCCEED();
}

TEST_F(AsyncLoadCoordinatorTest, RestartBeforeFirstCompletesCancelsFirst)
{
    TempFile tmp1(L"aload_first", "# first\n");
    TempFile tmp2(L"aload_second", "# second\n");

    AsyncLoadCoordinator c;
    c.Start(scheduler_, tmp1.PmrPath(), nullptr, 0, GetLightTheme());
    c.Start(scheduler_, tmp2.PmrPath(), nullptr, 0, GetLightTheme());

    std::optional<AsyncLoadResult> result;
    PollUntil([&] { result = c.TakeResult(); return result.has_value(); }, kPollTimeout);
    ASSERT_TRUE(result.has_value());
    EXPECT_FALSE(c.IsActive());
}
