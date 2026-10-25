#include <gtest/gtest.h>
#include "file_load_service.h"
#include "task_scheduler.h"
#include "test_helpers.h"
#include "theme.h"

class FileLoadServiceTest : public ::testing::Test {
protected:
    FileLoadService service_;
};

// preload と StartAsyncLoad を跨ぐキャンセル挙動は実 scheduler が要るため専用 fixture。
class FileLoadServicePreloadTest : public ::testing::Test {
protected:
    static TaskScheduler scheduler_;
    FileLoadService service_;

    static void SetUpTestSuite()
    {
        scheduler_.Init(1);
    }
    static void TearDownTestSuite()
    {
        scheduler_.Shutdown();
    }
};

TaskScheduler FileLoadServicePreloadTest::scheduler_;

TEST_F(FileLoadServiceTest, InitiallyNotLoading)
{
    EXPECT_FALSE(service_.IsLoading());
    EXPECT_FLOAT_EQ(service_.GetLoadingAngle(), 0.0f);
}

TEST_F(FileLoadServiceTest, BeginLoadingAnimation)
{
    service_.BeginLoadingAnimation();
    EXPECT_TRUE(service_.IsLoading());
    EXPECT_FLOAT_EQ(service_.GetLoadingAngle(), 0.0f);
}

TEST_F(FileLoadServiceTest, StopLoading)
{
    service_.BeginLoadingAnimation();
    service_.StopLoading();
    EXPECT_FALSE(service_.IsLoading());
}

TEST_F(FileLoadServiceTest, TickLoadingAnimation)
{
    service_.BeginLoadingAnimation();
    service_.TickLoadingAnimation();
    EXPECT_GT(service_.GetLoadingAngle(), 0.0f);
}

TEST_F(FileLoadServiceTest, TickLoadingAnimationWraps)
{
    service_.BeginLoadingAnimation();
    // 一周するのに十分な回数ティック（2*pi / 0.15 ≈ 42）
    for (int i = 0; i < 50; ++i) {
        service_.TickLoadingAnimation();
    }
    EXPECT_LE(service_.GetLoadingAngle(), TWO_PI);
}

TEST_F(FileLoadServiceTest, BeginLoadingAnimationResetsAngle)
{
    service_.BeginLoadingAnimation();
    for (int i = 0; i < 10; ++i) {
        service_.TickLoadingAnimation();
    }
    EXPECT_GT(service_.GetLoadingAngle(), 0.0f);

    service_.BeginLoadingAnimation();
    EXPECT_FLOAT_EQ(service_.GetLoadingAngle(), 0.0f);
}

TEST_F(FileLoadServiceTest, TakeAsyncReturnsNulloptWhenEmpty)
{
    // 結果がない状態で2回呼んでも安全
    EXPECT_FALSE(service_.TakeAsync().has_value());
    EXPECT_FALSE(service_.TakeAsync().has_value());
}

TEST_F(FileLoadServiceTest, CancelAsyncLoadDoesNotCrash)
{
    // 非同期ロードが進行中でなくてもキャンセルは安全
    service_.CancelAsyncLoad();
    EXPECT_FALSE(service_.TakeAsync().has_value());
}

TEST_F(FileLoadServicePreloadTest, StartAsyncLoadCancelsPreloadResult)
{
    TempFile preload_file(L"fls_preload", "# preload doc\n");
    TempFile new_file(L"fls_newload", "# new doc\n");

    service_.StartPreloadAsync(scheduler_, preload_file.PmrPath(), GetLightTheme());
    ASSERT_TRUE(PollUntil([&] { return service_.AttachPreload(nullptr, 0) == FileLoadService::PreloadAttachResult::AppliedSync; }));

    service_.StartAsyncLoad(scheduler_, new_file.PmrPath(), nullptr, 0, GetLightTheme(), 0.0f, {});
    EXPECT_EQ(service_.GetLoadingPath(), new_file.PmrPath());

    std::optional<AsyncLoadOutcome> outcome;
    PollUntil([&] { outcome = service_.TakeAsync(); return outcome.has_value(); });
    ASSERT_TRUE(outcome.has_value());
    ASSERT_TRUE(outcome->has_value());
    // preload 結果 (fls_preload) ではなく StartAsyncLoad の結果 (fls_newload) を返すこと。
    EXPECT_EQ((*outcome)->doc.GetFilePath(), new_file.PmrPath());
}
