#include <gtest/gtest.h>
#include "file_loader.h"
#include <fstream>
#include <filesystem>

namespace fs = std::filesystem;

class FileLoaderTest : public ::testing::Test {
protected:
    fs::path temp_dir_;

    void SetUp() override
    {
        wchar_t tmp[MAX_PATH];
        GetTempPathW(MAX_PATH, tmp);
        temp_dir_ = fs::path(tmp) / (L"mendo_test_" + std::to_wstring(GetCurrentProcessId()));
        fs::create_directories(temp_dir_);
    }

    void TearDown() override
    {
        fs::remove_all(temp_dir_);
    }

    fs::path WriteFile(std::wstring_view name, std::string_view content)
    {
        auto path = temp_dir_ / name;
        std::ofstream f(path, std::ios::binary);
        f.write(content.data(), content.size());
        f.close();
        return path;
    }

    // 非同期ファイル監視のポーリングを待つヘルパー
    bool PollForChange(FileLoader& loader, int max_ms = 2000)
    {
        for (int elapsed = 0; elapsed < max_ms; elapsed += 50) {
            loader.CheckForChanges();
            Sleep(50);
        }
        return true;
    }
};

TEST_F(FileLoaderTest, LoadsUtf8File)
{
    auto path = WriteFile(L"test.md", "Hello, World!");
    auto content = FileLoader::LoadFile(path.native().c_str());
    EXPECT_EQ(content, "Hello, World!");
}

TEST_F(FileLoaderTest, LoadsMultilineFile)
{
    auto path = WriteFile(L"multi.md", "line1\nline2\nline3");
    auto content = FileLoader::LoadFile(path.native().c_str());
    EXPECT_EQ(content, "line1\nline2\nline3");
}

TEST_F(FileLoaderTest, StripsUtf8Bom)
{
    std::string bom = "\xEF\xBB\xBF" "Hello";
    auto path = WriteFile(L"bom.md", bom);
    auto content = FileLoader::LoadFile(path.native().c_str());
    EXPECT_EQ(content, "Hello");
}

TEST_F(FileLoaderTest, NonExistentFileReturnsEmpty)
{
    auto content = FileLoader::LoadFile(L"C:\\nonexistent_file_12345.md");
    EXPECT_TRUE(content.empty());
}

TEST_F(FileLoaderTest, EmptyFileReturnsEmpty)
{
    auto path = WriteFile(L"empty.md", "");
    auto content = FileLoader::LoadFile(path.native().c_str());
    EXPECT_TRUE(content.empty());
}

TEST_F(FileLoaderTest, LoadsJapaneseUtf8)
{
    auto path = WriteFile(L"jp.md", "日本語テスト");
    auto content = FileLoader::LoadFile(path.native().c_str());
    EXPECT_EQ(content, "日本語テスト");
}

TEST_F(FileLoaderTest, BomOnlyFileReturnsEmpty)
{
    std::string bom_only = "\xEF\xBB\xBF";
    auto path = WriteFile(L"bomonly.md", bom_only);
    auto content = FileLoader::LoadFile(path.native().c_str());
    EXPECT_TRUE(content.empty());
}

// ---- ファイル監視テスト ----

TEST_F(FileLoaderTest, WatcherDetectsChange)
{
    auto path = WriteFile(L"watch.md", "original");

    FileLoader loader;
    bool changed = false;
    loader.StartWatching(path.native().c_str(), [&]() { changed = true; });

    // 異なるタイムスタンプを確保するため、少し待ってからファイルを変更
    Sleep(300);
    WriteFile(L"watch.md", "modified");

    // 非同期通知をポーリングで待つ
    for (int i = 0; i < 40 && !changed; i++) {
        Sleep(50);
        loader.CheckForChanges();
    }
    EXPECT_TRUE(changed);
}

TEST_F(FileLoaderTest, WatcherDoesNotFireWithoutChange)
{
    auto path = WriteFile(L"nochange.md", "content");

    FileLoader loader;
    bool changed = false;
    loader.StartWatching(path.native().c_str(), [&]() { changed = true; });

    // 少し待ってからチェック（変更なしなので発火しないはず）
    Sleep(100);
    loader.CheckForChanges();
    EXPECT_FALSE(changed);
}

TEST_F(FileLoaderTest, StopWatchingPreventsCallback)
{
    auto path = WriteFile(L"stop.md", "content");

    FileLoader loader;
    bool changed = false;
    loader.StartWatching(path.native().c_str(), [&]() { changed = true; });
    loader.StopWatching();

    Sleep(300);
    WriteFile(L"stop.md", "modified");
    Sleep(100);
    loader.CheckForChanges();
    EXPECT_FALSE(changed);
}

// ---- 追加のエッジケース ----

TEST_F(FileLoaderTest, LargeFile)
{
    // 1MBのファイルを作成
    std::string large_content(1024 * 1024, 'A');
    auto path = WriteFile(L"large.md", large_content);
    auto content = FileLoader::LoadFile(path.native().c_str());
    EXPECT_EQ(content.size(), large_content.size());
}

TEST_F(FileLoaderTest, FileWithOnlyBomAndContent)
{
    std::string bom_content = "\xEF\xBB\xBF# Title\n\nContent";
    auto path = WriteFile(L"bomcontent.md", bom_content);
    auto content = FileLoader::LoadFile(path.native().c_str());
    EXPECT_EQ(content, "# Title\n\nContent");
}

TEST_F(FileLoaderTest, WatcherRestartOnNewFile)
{
    auto path1 = WriteFile(L"watch1.md", "content1");
    auto path2 = WriteFile(L"watch2.md", "content2");

    FileLoader loader;
    int change_count = 0;
    loader.StartWatching(path1.native().c_str(), [&]() { change_count++; });

    // 別のファイルの監視に切り替え
    loader.StartWatching(path2.native().c_str(), [&]() { change_count++; });

    // 元のファイルを変更 - コールバックが発火しないこと
    Sleep(300);
    WriteFile(L"watch1.md", "modified1");
    // watch1の変更通知を拾いつつ、ファイル名フィルタで弾くことを確認
    for (int i = 0; i < 20; i++) {
        Sleep(50);
        loader.CheckForChanges();
    }
    EXPECT_EQ(change_count, 0);

    // 新しいファイルを変更 - コールバックが発火すること
    Sleep(300);
    WriteFile(L"watch2.md", "modified2");
    for (int i = 0; i < 40 && change_count == 0; i++) {
        Sleep(50);
        loader.CheckForChanges();
    }
    EXPECT_EQ(change_count, 1);
}

TEST_F(FileLoaderTest, WatcherDestructorDoesNotCrash)
{
    auto path = WriteFile(L"destructor.md", "content");
    {
        FileLoader loader;
        loader.StartWatching(path.native().c_str(), []() {});
        // デストラクタで安全に監視が停止されること
    }
}

TEST_F(FileLoaderTest, FileWithNewlines)
{
    auto path = WriteFile(L"newlines.md", "line1\r\nline2\r\nline3");
    auto content = FileLoader::LoadFile(path.native().c_str());
    EXPECT_EQ(content, "line1\r\nline2\r\nline3");
}

// ---- デバウンスリセットテスト ----

TEST_F(FileLoaderTest, ResetDebounceTickWithoutWatching)
{
    // 監視未開始でも安全に呼べること
    FileLoader loader;
    loader.ResetDebounceTick();
}

TEST_F(FileLoaderTest, ResetDebounceSuppressesDuplicateChange)
{
    auto path = WriteFile(L"debounce.md", "original");

    FileLoader loader;
    int change_count = 0;
    loader.StartWatching(path.native().c_str(), [&]() { change_count++; });

    // 初期デバウンスを過ぎるまで待つ
    Sleep(300);

    // ファイルを変更して検出を待つ
    WriteFile(L"debounce.md", "modified1");
    for (int i = 0; i < 40 && change_count == 0; i++) {
        Sleep(50);
        loader.CheckForChanges();
    }
    ASSERT_EQ(change_count, 1);

    // デバウンスをリセット（DoReloadCurrentFile完了をシミュレート）
    loader.ResetDebounceTick();

    // すぐにファイルを変更（OSの重複通知をシミュレート）
    WriteFile(L"debounce.md", "modified2");

    // デバウンス期間内（200ms以内）にポーリング → 抑制されるはず
    for (int i = 0; i < 3; i++) {
        Sleep(50);
        loader.CheckForChanges();
    }
    EXPECT_EQ(change_count, 1);
}

TEST_F(FileLoaderTest, ChangeDetectedAfterResetDebounceExpires)
{
    auto path = WriteFile(L"debounce2.md", "original");

    FileLoader loader;
    int change_count = 0;
    loader.StartWatching(path.native().c_str(), [&]() { change_count++; });

    // 初期デバウンスを過ぎるまで待つ
    Sleep(300);

    // デバウンスをリセット
    loader.ResetDebounceTick();

    // デバウンス期間が過ぎるのを待つ
    Sleep(300);

    // ファイルを変更 → デバウンス切れなので検出されるはず
    WriteFile(L"debounce2.md", "modified");
    for (int i = 0; i < 40 && change_count == 0; i++) {
        Sleep(50);
        loader.CheckForChanges();
    }
    EXPECT_EQ(change_count, 1);
}
