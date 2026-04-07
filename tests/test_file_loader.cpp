#include <gtest/gtest.h>
#include "file_loader.h"
#include "file_watcher.h"
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
    bool PollForChange(FileWatcher& watcher, int max_ms = 2000)
    {
        for (int elapsed = 0; elapsed < max_ms; elapsed += 50) {
            watcher.CheckForChanges();
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

    FileWatcher watcher;
    bool changed = false;
    watcher.StartWatching(path.native().c_str(), [&]() { changed = true; });

    // 異なるタイムスタンプを確保するため、少し待ってからファイルを変更
    Sleep(300);
    WriteFile(L"watch.md", "modified");

    // 非同期通知をポーリングで待つ
    for (int i = 0; i < 40 && !changed; i++) {
        Sleep(50);
        watcher.CheckForChanges();
    }
    EXPECT_TRUE(changed);
}

TEST_F(FileLoaderTest, WatcherDoesNotFireWithoutChange)
{
    auto path = WriteFile(L"nochange.md", "content");

    FileWatcher watcher;
    bool changed = false;
    watcher.StartWatching(path.native().c_str(), [&]() { changed = true; });

    // 少し待ってからチェック（変更なしなので発火しないはず）
    Sleep(100);
    watcher.CheckForChanges();
    EXPECT_FALSE(changed);
}

TEST_F(FileLoaderTest, StopWatchingPreventsCallback)
{
    auto path = WriteFile(L"stop.md", "content");

    FileWatcher watcher;
    bool changed = false;
    watcher.StartWatching(path.native().c_str(), [&]() { changed = true; });
    watcher.StopWatching();

    Sleep(300);
    WriteFile(L"stop.md", "modified");
    Sleep(100);
    watcher.CheckForChanges();
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

    FileWatcher watcher;
    int change_count = 0;
    watcher.StartWatching(path1.native().c_str(), [&]() { change_count++; });

    // 別のファイルの監視に切り替え
    watcher.StartWatching(path2.native().c_str(), [&]() { change_count++; });

    // 元のファイルを変更 - コールバックが発火しないこと
    Sleep(300);
    WriteFile(L"watch1.md", "modified1");
    // watch1の変更通知を拾いつつ、ファイル名フィルタで弾くことを確認
    for (int i = 0; i < 20; i++) {
        Sleep(50);
        watcher.CheckForChanges();
    }
    EXPECT_EQ(change_count, 0);

    // 新しいファイルを変更 - コールバックが発火すること
    Sleep(300);
    WriteFile(L"watch2.md", "modified2");
    for (int i = 0; i < 40 && change_count == 0; i++) {
        Sleep(50);
        watcher.CheckForChanges();
    }
    EXPECT_EQ(change_count, 1);
}

TEST_F(FileLoaderTest, WatcherDestructorDoesNotCrash)
{
    auto path = WriteFile(L"destructor.md", "content");
    {
        FileWatcher watcher;
        watcher.StartWatching(path.native().c_str(), []() static {});
        // デストラクタで安全に監視が停止されること
    }
}

TEST_F(FileLoaderTest, FileWithNewlines)
{
    auto path = WriteFile(L"newlines.md", "line1\r\nline2\r\nline3");
    auto content = FileLoader::LoadFile(path.native().c_str());
    EXPECT_EQ(content, "line1\r\nline2\r\nline3");
}

// ---- 監視一時停止 / ResumeWatching テスト ----

TEST_F(FileLoaderTest, ResumeWatchingWithoutWatching)
{
    // 監視未開始でも安全に呼べること
    FileWatcher watcher;
    watcher.ResumeWatching();
}

TEST_F(FileLoaderTest, WatchPausedAfterChangeDetected)
{
    auto path = WriteFile(L"pause.md", "original");

    FileWatcher watcher;
    int change_count = 0;
    watcher.StartWatching(path.native().c_str(), [&]() { change_count++; });

    // ファイルを変更して検出を待つ
    Sleep(300);
    WriteFile(L"pause.md", "modified1");
    for (int i = 0; i < 40 && change_count == 0; i++) {
        Sleep(50);
        watcher.CheckForChanges();
    }
    ASSERT_EQ(change_count, 1);

    // 変更検出後は一時停止（コールバック抑制、I/Oは継続）
    // 追加の保存はコールバックを呼ばず蓄積される
    Sleep(300);
    WriteFile(L"pause.md", "modified2");
    for (int i = 0; i < 40; i++) {
        Sleep(50);
        watcher.CheckForChanges();
    }
    EXPECT_EQ(change_count, 1);

    // ResumeWatching で蓄積された変更が通知される
    watcher.ResumeWatching();
    EXPECT_EQ(change_count, 2);
}

TEST_F(FileLoaderTest, ResumeWatchingReenablesDetection)
{
    auto path = WriteFile(L"resume.md", "original");

    FileWatcher watcher;
    int change_count = 0;
    watcher.StartWatching(path.native().c_str(), [&]() { change_count++; });

    // 最初の変更を検出
    Sleep(300);
    WriteFile(L"resume.md", "modified1");
    for (int i = 0; i < 40 && change_count == 0; i++) {
        Sleep(50);
        watcher.CheckForChanges();
    }
    ASSERT_EQ(change_count, 1);

    // 監視を再開（リロード完了をシミュレート）
    watcher.ResumeWatching();
    EXPECT_NE(watcher.GetEventHandle(), nullptr);

    // 2回目の変更を検出
    Sleep(300);
    WriteFile(L"resume.md", "modified2");
    for (int i = 0; i < 40 && change_count == 1; i++) {
        Sleep(50);
        watcher.CheckForChanges();
    }
    EXPECT_EQ(change_count, 2);
}

// ---- GetEventHandle テスト ----

TEST_F(FileLoaderTest, GetEventHandleNullWhenNotWatching)
{
    FileWatcher watcher;
    EXPECT_EQ(watcher.GetEventHandle(), nullptr);
}

TEST_F(FileLoaderTest, GetEventHandleValidWhileWatching)
{
    auto path = WriteFile(L"evthandle.md", "content");
    FileWatcher watcher;
    watcher.StartWatching(path.native().c_str(), []() static {});
    EXPECT_NE(watcher.GetEventHandle(), nullptr);
}

TEST_F(FileLoaderTest, GetEventHandleNullAfterStopWatching)
{
    auto path = WriteFile(L"evtstop.md", "content");
    FileWatcher watcher;
    watcher.StartWatching(path.native().c_str(), []() static {});
    EXPECT_NE(watcher.GetEventHandle(), nullptr);
    watcher.StopWatching();
    EXPECT_EQ(watcher.GetEventHandle(), nullptr);
}
