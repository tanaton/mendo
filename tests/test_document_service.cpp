#include <gtest/gtest.h>
#include <memory_resource>
#include <string_view>
#include "document_service.h"
#include "file_watcher.h"
#include "test_helpers.h"
#include <fstream>
#include <filesystem>

class DocumentServiceTest : public TempDirTestBase {
protected:
    std::pmr::wstring CreateTestFile(const std::string& name, const std::string& content)
    {
        const auto path = WriteTempFile(std::filesystem::path(name).wstring(), content);
        return std::pmr::wstring{ path.wstring() };
    }

    FileWatcher watcher_;
};

TEST_F(DocumentServiceTest, LoadFileSuccess)
{
    auto path = CreateTestFile("test.md", "# Hello\nWorld");
    DocumentService service(watcher_);

    auto result = service.LoadFile(path);
    ASSERT_TRUE(result.has_value());
    EXPECT_FALSE(result->IsEmpty());
    EXPECT_EQ(std::wstring_view{ result->GetFilePath() }, std::wstring_view{ path });
    EXPECT_FALSE(result->GetToc().GetEntries().empty());
}

TEST_F(DocumentServiceTest, LoadFileNotFound)
{
    DocumentService service(watcher_);

    std::pmr::wstring nonexistent{ L"C:\\nonexistent\\file.md" };
    auto result = service.LoadFile(nonexistent);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error(), FileLoadError::NotFound);
}

TEST_F(DocumentServiceTest, NeedsAsyncLoadSmallFile)
{
    auto path = CreateTestFile("small.md", "# Small");
    EXPECT_FALSE(DocumentService::NeedsAsyncLoad(path));
}

TEST_F(DocumentServiceTest, NeedsAsyncLoadLargeFile)
{
    // 64KB超のファイルは非同期ロード判定
    std::string content(65 * 1024, 'x');
    auto path = CreateTestFile("large.md", content);
    EXPECT_TRUE(DocumentService::NeedsAsyncLoad(path));
}

TEST_F(DocumentServiceTest, NeedsAsyncLoadNonexistent)
{
    auto ws = (temp_dir_ / "nonexistent_async.md").wstring();
    std::pmr::wstring nonexistent{ ws };
    EXPECT_TRUE(DocumentService::NeedsAsyncLoad(nonexistent));
}

TEST_F(DocumentServiceTest, NeedsLoadingAnimationSmallFile)
{
    auto path = CreateTestFile("small.md", "# Small");
    EXPECT_FALSE(DocumentService::NeedsLoadingAnimation(path));
}

TEST_F(DocumentServiceTest, NeedsLoadingAnimationNonexistent)
{
    auto ws = (temp_dir_ / "nonexistent_anim.md").wstring();
    std::pmr::wstring nonexistent{ ws };
    EXPECT_TRUE(DocumentService::NeedsLoadingAnimation(nonexistent));
}

TEST_F(DocumentServiceTest, ResumeWatching)
{
    // DocumentService経由でResumeWatchingが安全に呼べること
    DocumentService service(watcher_);
    service.ResumeWatching();
}
