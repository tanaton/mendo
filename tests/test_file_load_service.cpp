#include <gtest/gtest.h>
#include "file_load_service.h"
#include "test_helpers.h"
#include "theme.h"

class FileLoadServiceTest : public ::testing::Test {
protected:
    FileLoadService service_;
};

namespace {
constexpr auto kPollTimeout = std::chrono::seconds(5);
} // namespace

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

TEST_F(FileLoadServiceTest, StartLoading)
{
    service_.StartLoading(L"test.md");
    EXPECT_TRUE(service_.IsLoading());
    EXPECT_FLOAT_EQ(service_.GetLoadingAngle(), 0.0f);
    EXPECT_EQ(service_.GetLoadingPath(), L"test.md");
}

TEST_F(FileLoadServiceTest, StopLoading)
{
    service_.StartLoading(L"test.md");
    service_.StopLoading();
    EXPECT_FALSE(service_.IsLoading());
}

TEST_F(FileLoadServiceTest, TickLoadingAnimation)
{
    service_.StartLoading(L"test.md");
    service_.TickLoadingAnimation();
    EXPECT_GT(service_.GetLoadingAngle(), 0.0f);
}

TEST_F(FileLoadServiceTest, TickLoadingAnimationWraps)
{
    service_.StartLoading(L"test.md");
    // 一周するのに十分な回数ティック（2*pi / 0.15 ≈ 42）
    for (int i = 0; i < 50; ++i) {
        service_.TickLoadingAnimation();
    }
    // ラップ後の角度は2*pi未満であること
    EXPECT_LT(service_.GetLoadingAngle(), 6.2831853f);
}

TEST_F(FileLoadServiceTest, SetLoadingPath)
{
    service_.SetLoadingPath(L"path/to/file.md");
    EXPECT_EQ(service_.GetLoadingPath(), L"path/to/file.md");
}

TEST_F(FileLoadServiceTest, ExecuteLoadNonexistentFile)
{
    Document doc;
    LayoutCache cache;
    service_.SetLoadingPath(L"nonexistent_file.md");
    auto result = service_.ExecuteLoad(doc, cache);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error(), FileLoadError::NotFound);
    EXPECT_FALSE(service_.IsLoading());
}

TEST_F(FileLoadServiceTest, LoadStopsAnimation)
{
    service_.StartLoading(L"nonexistent.md");
    EXPECT_TRUE(service_.IsLoading());

    Document doc;
    LayoutCache cache;
    (void)service_.ExecuteLoad(doc, cache);
    EXPECT_FALSE(service_.IsLoading());
}

TEST_F(FileLoadServiceTest, StartLoadingResetsAngle)
{
    // 2回目のStartLoadingでアングルがリセットされる
    service_.StartLoading(L"first.md");
    for (int i = 0; i < 10; ++i) {
        service_.TickLoadingAnimation();
    }
    EXPECT_GT(service_.GetLoadingAngle(), 0.0f);

    service_.StartLoading(L"second.md");
    EXPECT_FLOAT_EQ(service_.GetLoadingAngle(), 0.0f);
}

TEST_F(FileLoadServiceTest, TakeAsyncResultReturnsNulloptWhenEmpty)
{
    auto result = service_.TakeAsyncResult();
    EXPECT_FALSE(result.has_value());
}

TEST_F(FileLoadServiceTest, TakeAsyncResultReturnsNulloptAfterConsume)
{
    // 結果がない状態で2回呼んでも安全
    EXPECT_FALSE(service_.TakeAsyncResult().has_value());
    EXPECT_FALSE(service_.TakeAsyncResult().has_value());
}

TEST_F(FileLoadServiceTest, CancelAsyncLoadDoesNotCrash)
{
    // 非同期ロードが進行中でなくてもキャンセルは安全
    service_.CancelAsyncLoad();
    EXPECT_FALSE(service_.TakeAsyncResult().has_value());
}

TEST_F(FileLoadServicePreloadTest, StartAsyncLoadCancelsPreloadResult)
{
    TempFile preload_file(L"fls_preload", "# preload doc\n");
    TempFile new_file(L"fls_newload", "# new doc\n");

    service_.StartPreloadAsync(preload_file.PmrPath());
    // preload worker が結果を sink に積んで cv.wait (hwnd 待ち) に入るまで待つ。
    std::this_thread::sleep_for(std::chrono::milliseconds(150));

    service_.SetLoadingPath(new_file.PmrPath());
    service_.StartAsyncLoad(scheduler_, nullptr, 0, GetLightTheme());

    std::optional<AsyncLoadResult> result;
    PollUntil([&] { result = service_.TakeAsyncResult(); return result.has_value(); }, kPollTimeout);
    ASSERT_TRUE(result.has_value());
    // preload 結果 (fls_preload) ではなく StartAsyncLoad の結果 (fls_newload) を返すこと。
    EXPECT_EQ(result->doc.GetFilePath(), new_file.PmrPath());
}
