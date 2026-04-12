#include <gtest/gtest.h>
#include "file_load_service.h"
#include "file_watcher.h"

class FileLoadServiceTest : public ::testing::Test {
protected:
    FileWatcher watcher_;
    DocumentService doc_service_{ watcher_ };
    FileLoadService service_{ doc_service_ };
};

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
