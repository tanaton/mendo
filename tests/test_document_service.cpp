#include <gtest/gtest.h>
#include <memory_resource>
#include <string_view>
#include "document_service.h"
#include "file_watcher.h"
#include <fstream>
#include <filesystem>

class DocumentServiceTest : public ::testing::Test {
protected:
    void SetUp() override
    {
        test_dir_ = std::filesystem::temp_directory_path() / "mendo_doc_service_test";
        std::filesystem::create_directories(test_dir_);
    }
    void TearDown() override
    {
        std::filesystem::remove_all(test_dir_);
    }

    std::pmr::wstring CreateTestFile(const std::string& name, const std::string& content)
    {
        auto path = test_dir_ / name;
        std::ofstream ofs(path, std::ios::binary);
        ofs << content;
        auto ws = path.wstring();
        return std::pmr::wstring{ std::wstring_view{ws} };
    }

    std::filesystem::path test_dir_;
    FileWatcher watcher_;
};

TEST_F(DocumentServiceTest, LoadFileSuccess)
{
    auto path = CreateTestFile("test.md", "# Hello\nWorld");
    DocumentService service(watcher_);
    Document doc;

    EXPECT_TRUE(service.LoadFile(path, doc));
    EXPECT_FALSE(doc.IsEmpty());
    EXPECT_EQ(std::wstring_view{ doc.GetFilePath() }, std::wstring_view{ path });
    EXPECT_FALSE(doc.GetToc().GetEntries().empty());
}

TEST_F(DocumentServiceTest, LoadFileNotFound)
{
    DocumentService service(watcher_);
    Document doc;

    std::pmr::wstring nonexistent{ L"C:\\nonexistent\\file.md" };
    EXPECT_FALSE(service.LoadFile(nonexistent, doc));
    EXPECT_TRUE(doc.IsEmpty());
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
    std::pmr::wstring nonexistent{ L"C:\\nonexistent\\file.md" };
    EXPECT_TRUE(DocumentService::NeedsAsyncLoad(nonexistent));
}

TEST_F(DocumentServiceTest, NeedsLoadingAnimationSmallFile)
{
    auto path = CreateTestFile("small.md", "# Small");
    EXPECT_FALSE(DocumentService::NeedsLoadingAnimation(path));
}

TEST_F(DocumentServiceTest, NeedsLoadingAnimationNonexistent)
{
    // 存在しないファイルはアニメーション必要と報告（サイズを判定できないため）
    std::pmr::wstring nonexistent{ L"C:\\nonexistent\\file.md" };
    EXPECT_TRUE(DocumentService::NeedsLoadingAnimation(nonexistent));
}

TEST_F(DocumentServiceTest, ResumeWatching)
{
    // DocumentService経由でResumeWatchingが安全に呼べること
    DocumentService service(watcher_);
    service.ResumeWatching();
}
