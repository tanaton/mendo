#include <gtest/gtest.h>
#include "file_load_service.h"

class FileLoadServiceTest : public ::testing::Test {
protected:
    FileLoader loader_;
    DocumentService doc_service_{loader_};
    FileLoadService service_{doc_service_};
};

TEST_F(FileLoadServiceTest, InitiallyNotLoading) {
    EXPECT_FALSE(service_.IsLoading());
    EXPECT_FLOAT_EQ(service_.GetLoadingAngle(), 0.0f);
}

TEST_F(FileLoadServiceTest, StartLoading) {
    service_.StartLoading(L"test.md");
    EXPECT_TRUE(service_.IsLoading());
    EXPECT_FLOAT_EQ(service_.GetLoadingAngle(), 0.0f);
    EXPECT_EQ(service_.GetLoadingPath(), L"test.md");
}

TEST_F(FileLoadServiceTest, StopLoading) {
    service_.StartLoading(L"test.md");
    service_.StopLoading();
    EXPECT_FALSE(service_.IsLoading());
}

TEST_F(FileLoadServiceTest, TickLoadingAnimation) {
    service_.StartLoading(L"test.md");
    service_.TickLoadingAnimation();
    EXPECT_GT(service_.GetLoadingAngle(), 0.0f);
}

TEST_F(FileLoadServiceTest, TickLoadingAnimationWraps) {
    service_.StartLoading(L"test.md");
    // Tick enough times to wrap around (2*pi / 0.15 ≈ 42)
    for (int i = 0; i < 50; ++i) {
        service_.TickLoadingAnimation();
    }
    // Angle should be less than 2*pi after wrapping
    EXPECT_LT(service_.GetLoadingAngle(), 6.2831853f);
}

TEST_F(FileLoadServiceTest, SetLoadingPath) {
    service_.SetLoadingPath(L"path/to/file.md");
    EXPECT_EQ(service_.GetLoadingPath(), L"path/to/file.md");
}

TEST_F(FileLoadServiceTest, ExecuteLoadNonexistentFile) {
    Document doc;
    LayoutCache cache;
    service_.SetLoadingPath(L"nonexistent_file.md");
    bool result = service_.ExecuteLoad(doc, cache);
    EXPECT_FALSE(result);
    EXPECT_FALSE(service_.IsLoading());
}

TEST_F(FileLoadServiceTest, ExecuteReloadEmptyPath) {
    Document doc;
    LayoutCache cache;
    bool result = service_.ExecuteReload(doc, cache);
    EXPECT_FALSE(result);
}

TEST_F(FileLoadServiceTest, LoadStopsAnimation) {
    service_.StartLoading(L"nonexistent.md");
    EXPECT_TRUE(service_.IsLoading());

    Document doc;
    LayoutCache cache;
    service_.ExecuteLoad(doc, cache);
    EXPECT_FALSE(service_.IsLoading());
}
