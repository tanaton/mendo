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
        temp_dir_ = fs::path(tmp) / L"mendo_test";
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

// ---- Additional edge cases ----

TEST_F(FileLoaderTest, LargeFile) {
    // Create a 1MB file
    std::string large_content(1024 * 1024, 'A');
    auto path = WriteFile(L"large.md", large_content);
    auto content = FileLoader::LoadFile(path.wstring());
    EXPECT_EQ(content.size(), large_content.size());
}

TEST_F(FileLoaderTest, FileWithOnlyBomAndContent) {
    std::string bom_content = "\xEF\xBB\xBF# Title\n\nContent";
    auto path = WriteFile(L"bomcontent.md", bom_content);
    auto content = FileLoader::LoadFile(path.wstring());
    EXPECT_EQ(content, "# Title\n\nContent");
}

TEST_F(FileLoaderTest, WatcherRestartOnNewFile) {
    auto path1 = WriteFile(L"watch1.md", "content1");
    auto path2 = WriteFile(L"watch2.md", "content2");

    FileLoader loader;
    int change_count = 0;
    loader.StartWatching(path1.wstring(), [&]() { change_count++; });

    // Switch to watching a different file
    loader.StartWatching(path2.wstring(), [&]() { change_count++; });

    // Modify original file - should NOT trigger callback
    Sleep(300);
    WriteFile(L"watch1.md", "modified1");
    loader.CheckForChanges();
    EXPECT_EQ(change_count, 0);

    // Modify new file - should trigger callback
    Sleep(300);
    WriteFile(L"watch2.md", "modified2");
    loader.CheckForChanges();
    EXPECT_EQ(change_count, 1);
}

TEST_F(FileLoaderTest, WatcherDestructorDoesNotCrash) {
    auto path = WriteFile(L"destructor.md", "content");
    {
        FileLoader loader;
        loader.StartWatching(path.wstring(), []() {});
        // Destructor should safely stop watching
    }
}

TEST_F(FileLoaderTest, FileWithNewlines) {
    auto path = WriteFile(L"newlines.md", "line1\r\nline2\r\nline3");
    auto content = FileLoader::LoadFile(path.wstring());
    EXPECT_EQ(content, "line1\r\nline2\r\nline3");
}
