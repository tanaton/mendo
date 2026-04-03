#include <gtest/gtest.h>
#include <memory_resource>
#include <string_view>
#include "document_service.h"
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
    FileLoader loader_;
};

TEST_F(DocumentServiceTest, LoadFileSuccess)
{
    auto path = CreateTestFile("test.md", "# Hello\nWorld");
    DocumentService service(loader_);
    Document doc;

    EXPECT_TRUE(service.LoadFile(path, doc));
    EXPECT_FALSE(doc.IsEmpty());
    EXPECT_EQ(std::wstring_view{ doc.GetFilePath() }, std::wstring_view{ path });
    EXPECT_FALSE(doc.GetToc().GetEntries().empty());
}

TEST_F(DocumentServiceTest, LoadFileNotFound)
{
    DocumentService service(loader_);
    Document doc;

    std::pmr::wstring nonexistent{ L"C:\\nonexistent\\file.md" };
    EXPECT_FALSE(service.LoadFile(nonexistent, doc));
    EXPECT_TRUE(doc.IsEmpty());
}

TEST_F(DocumentServiceTest, ReloadFile)
{
    auto path = CreateTestFile("reload.md", "# First");
    DocumentService service(loader_);
    Document doc;

    EXPECT_TRUE(service.LoadFile(path, doc));
    EXPECT_EQ(doc.GetToc().GetEntries()[0].text, L"First");

    // ファイルを変更
    {
        std::ofstream ofs(std::filesystem::path(path.c_str()), std::ios::binary);
        ofs << "# Second\n## Sub";
    }

    EXPECT_TRUE(service.ReloadFile(doc));
    EXPECT_EQ(std::wstring_view{ doc.GetFilePath() }, std::wstring_view{ path });
    EXPECT_GE(doc.GetToc().GetEntries().size(), 2u);
    EXPECT_EQ(doc.GetToc().GetEntries()[0].text, L"Second");
}

TEST_F(DocumentServiceTest, ReloadEmptyPathFails)
{
    DocumentService service(loader_);
    Document doc;

    EXPECT_FALSE(service.ReloadFile(doc));
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

TEST_F(DocumentServiceTest, ResetDebounceTick)
{
    // DocumentService経由でResetDebounceTickが安全に呼べること
    DocumentService service(loader_);
    service.ResetDebounceTick();
}
