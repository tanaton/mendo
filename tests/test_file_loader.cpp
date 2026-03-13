#include <gtest/gtest.h>
#include "file_loader.h"
#include <fstream>
#include <filesystem>

namespace fs = std::filesystem;

class FileLoaderTest : public ::testing::Test {
protected:
    fs::path temp_dir_;

    void SetUp() override {
        wchar_t tmp[MAX_PATH];
        GetTempPathW(MAX_PATH, tmp);
        temp_dir_ = fs::path(tmp) / L"mdviewer_test";
        fs::create_directories(temp_dir_);
    }

    void TearDown() override {
        fs::remove_all(temp_dir_);
    }

    fs::path WriteFile(const std::wstring& name, const std::string& content) {
        auto path = temp_dir_ / name;
        std::ofstream f(path, std::ios::binary);
        f.write(content.data(), content.size());
        f.close();
        return path;
    }
};

TEST_F(FileLoaderTest, LoadsUtf8File) {
    auto path = WriteFile(L"test.md", "Hello, World!");
    auto content = FileLoader::LoadFile(path.wstring());
    EXPECT_EQ(content, "Hello, World!");
}

TEST_F(FileLoaderTest, LoadsMultilineFile) {
    auto path = WriteFile(L"multi.md", "line1\nline2\nline3");
    auto content = FileLoader::LoadFile(path.wstring());
    EXPECT_EQ(content, "line1\nline2\nline3");
}

TEST_F(FileLoaderTest, StripsUtf8Bom) {
    std::string bom = "\xEF\xBB\xBF" "Hello";
    auto path = WriteFile(L"bom.md", bom);
    auto content = FileLoader::LoadFile(path.wstring());
    EXPECT_EQ(content, "Hello");
}

TEST_F(FileLoaderTest, NonExistentFileReturnsEmpty) {
    auto content = FileLoader::LoadFile(L"C:\\nonexistent_file_12345.md");
    EXPECT_TRUE(content.empty());
}

TEST_F(FileLoaderTest, EmptyFileReturnsEmpty) {
    auto path = WriteFile(L"empty.md", "");
    auto content = FileLoader::LoadFile(path.wstring());
    EXPECT_TRUE(content.empty());
}

TEST_F(FileLoaderTest, LoadsJapaneseUtf8) {
    auto path = WriteFile(L"jp.md", "日本語テスト");
    auto content = FileLoader::LoadFile(path.wstring());
    EXPECT_EQ(content, "日本語テスト");
}

TEST_F(FileLoaderTest, BomOnlyFileReturnsEmpty) {
    std::string bom_only = "\xEF\xBB\xBF";
    auto path = WriteFile(L"bomonly.md", bom_only);
    auto content = FileLoader::LoadFile(path.wstring());
    EXPECT_TRUE(content.empty());
}

// ---- File watcher tests ----

TEST_F(FileLoaderTest, WatcherDetectsChange) {
    auto path = WriteFile(L"watch.md", "original");

    FileLoader loader;
    bool changed = false;
    loader.StartWatching(path.wstring(), [&]() { changed = true; });

    // Modify file after a small delay to ensure different timestamp
    Sleep(300);
    WriteFile(L"watch.md", "modified");

    // Poll for changes
    loader.CheckForChanges();
    EXPECT_TRUE(changed);
}

TEST_F(FileLoaderTest, WatcherDoesNotFireWithoutChange) {
    auto path = WriteFile(L"nochange.md", "content");

    FileLoader loader;
    bool changed = false;
    loader.StartWatching(path.wstring(), [&]() { changed = true; });

    loader.CheckForChanges();
    EXPECT_FALSE(changed);
}

TEST_F(FileLoaderTest, StopWatchingPreventsCallback) {
    auto path = WriteFile(L"stop.md", "content");

    FileLoader loader;
    bool changed = false;
    loader.StartWatching(path.wstring(), [&]() { changed = true; });
    loader.StopWatching();

    Sleep(300);
    WriteFile(L"stop.md", "modified");
    loader.CheckForChanges();
    EXPECT_FALSE(changed);
}
