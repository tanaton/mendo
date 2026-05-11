#include <gtest/gtest.h>
#include <memory_resource>
#include <string_view>
#include "app_constants.h"
#include "document_service.h"
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
};

TEST_F(DocumentServiceTest, LoadFileSuccess)
{
    auto path = CreateTestFile("test.md", "# Hello\nWorld");

    auto result = DocumentService::LoadFile(path);
    ASSERT_TRUE(result.has_value());
    EXPECT_FALSE(result->IsEmpty());
    EXPECT_EQ(std::wstring_view{ result->GetFilePath() }, std::wstring_view{ path });
    EXPECT_FALSE(result->GetToc().GetEntries().empty());
}

TEST_F(DocumentServiceTest, LoadFileNotFound)
{
    std::pmr::wstring nonexistent{ L"C:\\nonexistent\\file.md" };
    auto result = DocumentService::LoadFile(nonexistent);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error(), FileLoadError::NotFound);
}

TEST_F(DocumentServiceTest, IsAsyncLoadCandidateSmallFile)
{
    auto path = CreateTestFile("small.md", "# Small");
    EXPECT_FALSE(DocumentService::IsAsyncLoadCandidate(path));
}

TEST_F(DocumentServiceTest, IsAsyncLoadCandidateLargeFile)
{
    // 64KB超のファイルは非同期ロード判定
    std::string content(65 * 1024, 'x');
    auto path = CreateTestFile("large.md", content);
    EXPECT_TRUE(DocumentService::IsAsyncLoadCandidate(path));
}

TEST_F(DocumentServiceTest, IsAsyncLoadCandidateNonexistent)
{
    auto ws = (temp_dir_ / "nonexistent_async.md").wstring();
    std::pmr::wstring nonexistent{ ws };
    EXPECT_TRUE(DocumentService::IsAsyncLoadCandidate(nonexistent));
}

TEST_F(DocumentServiceTest, ShouldShowLoadingAnimationSmallFile)
{
    auto path = CreateTestFile("small.md", "# Small");
    EXPECT_FALSE(DocumentService::ShouldShowLoadingAnimation(path));
}

TEST_F(DocumentServiceTest, ShouldShowLoadingAnimationNonexistent)
{
    auto ws = (temp_dir_ / "nonexistent_anim.md").wstring();
    std::pmr::wstring nonexistent{ ws };
    EXPECT_TRUE(DocumentService::ShouldShowLoadingAnimation(nonexistent));
}

