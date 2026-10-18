#include <gtest/gtest.h>
#include <memory_resource>
#include <string>
#include <string_view>
#include "document_service.h"
#include "test_helpers.h"

class DocumentServiceTest : public TempDirTestBase {
protected:
    std::pmr::wstring WriteDoc(std::wstring_view name, std::string_view content)
    {
        return std::pmr::wstring{ WriteTempFile(name, content).native() };
    }
    std::pmr::wstring MissingPath() const
    {
        return std::pmr::wstring{ (temp_dir_ / L"missing.md").native() };
    }
};

TEST_F(DocumentServiceTest, LoadFileSuccess)
{
    const auto path = WriteDoc(L"test.md", "# Hello\nWorld");

    auto result = DocumentService::LoadFile(path);
    ASSERT_TRUE(result.has_value());
    EXPECT_FALSE(result->IsEmpty());
    EXPECT_EQ(std::wstring_view{ result->GetFilePath() }, std::wstring_view{ path });
    EXPECT_FALSE(result->GetToc().GetEntries().empty());
}

// 親フォルダはあるがファイルが無い場合と、親フォルダごと無い場合 (ERROR_PATH_NOT_FOUND)。
TEST_F(DocumentServiceTest, LoadFileNotFound)
{
    const std::pmr::wstring kPaths[] = {
        MissingPath(),
        std::pmr::wstring{ (temp_dir_ / L"no_such_dir" / L"file.md").native() },
    };
    for (const auto& path : kPaths) {
        SCOPED_TRACE(::testing::PrintToString(std::wstring{ path }));
        auto result = DocumentService::LoadFile(path);
        ASSERT_FALSE(result.has_value());
        EXPECT_EQ(result.error(), FileLoadError::NotFound);
    }
}

// 64KB を超えるファイルと、サイズを取れないパスは非同期ロードに回す。
TEST_F(DocumentServiceTest, IsAsyncLoadCandidateBySize)
{
    EXPECT_FALSE(DocumentService::IsAsyncLoadCandidate(WriteDoc(L"small.md", "# Small")));
    EXPECT_TRUE(DocumentService::IsAsyncLoadCandidate(WriteDoc(L"large.md", std::string(65 * 1024, 'x'))));
    EXPECT_TRUE(DocumentService::IsAsyncLoadCandidate(MissingPath()));
}

TEST_F(DocumentServiceTest, ShouldShowLoadingAnimationBySize)
{
    EXPECT_FALSE(DocumentService::ShouldShowLoadingAnimation(WriteDoc(L"small.md", "# Small")));
    EXPECT_TRUE(DocumentService::ShouldShowLoadingAnimation(MissingPath()));
}
