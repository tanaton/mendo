#include <gtest/gtest.h>
#include "file_loader.h"
#include "file_watcher.h"
#include "test_helpers.h"
#include <filesystem>

namespace fs = std::filesystem;

class FileLoaderTest : public TempDirTestBase {
protected:
    fs::path WriteFile(std::wstring_view name, std::string_view content)
    {
        return WriteTempFile(name, content);
    }

    // イベントハンドルを使って変更通知を待つヘルパー
    void WaitForEvent(FileWatcher& watcher, int timeout_ms = 2000)
    {
        HANDLE h = watcher.GetEventHandle();
        if (h) {
            WaitForSingleObject(h, static_cast<DWORD>(timeout_ms));
        }
        watcher.CheckForChanges();
    }
};

TEST_F(FileLoaderTest, LoadsUtf8File)
{
    auto path = WriteFile(L"test.md", "Hello, World!");
    auto result = FileLoader::LoadFile(path.native().c_str());
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->text, "Hello, World!");
    EXPECT_EQ(result->byte_size, 13u);
}

TEST_F(FileLoaderTest, LoadsMultilineFile)
{
    auto path = WriteFile(L"multi.md", "line1\nline2\nline3");
    auto result = FileLoader::LoadFile(path.native().c_str());
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->text, "line1\nline2\nline3");
}

// FileLoader は UTF-8 BOM 除去のみ行う。byte_size は BOM 含む元サイズ。
TEST_F(FileLoaderTest, StripsUtf8Bom)
{
    auto path = WriteFile(L"bom.md", "\xEF\xBB\xBF" "Hello");
    auto result = FileLoader::LoadFile(path.native().c_str());
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->text, "Hello");
    EXPECT_EQ(result->byte_size, 8u);
}

TEST_F(FileLoaderTest, NonExistentFileReturnsError)
{
    auto result = FileLoader::LoadFile(L"C:\\nonexistent_file_12345.md");
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error(), FileLoadError::NotFound);
}

TEST_F(FileLoaderTest, EmptyFileReturnsEmpty)
{
    auto path = WriteFile(L"empty.md", "");
    auto result = FileLoader::LoadFile(path.native().c_str());
    ASSERT_TRUE(result.has_value());
    EXPECT_TRUE(result->text.empty());
    EXPECT_EQ(result->byte_size, 0u);
}

TEST_F(FileLoaderTest, LoadsJapaneseUtf8)
{
    auto path = WriteFile(L"jp.md", "日本語テスト");
    auto result = FileLoader::LoadFile(path.native().c_str());
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->text, "日本語テスト");
}

TEST_F(FileLoaderTest, BomOnlyFileReturnsEmpty)
{
    auto path = WriteFile(L"bomonly.md", "\xEF\xBB\xBF");
    auto result = FileLoader::LoadFile(path.native().c_str());
    ASSERT_TRUE(result.has_value());
    EXPECT_TRUE(result->text.empty());
    EXPECT_EQ(result->byte_size, 3u);
}

// ---- ファイル監視テスト ----

TEST_F(FileLoaderTest, WatcherDetectsChange)
{
    auto path = WriteFile(L"watch.md", "original");

    FileWatcher watcher;
    bool changed = false;
    watcher.StartWatching(path.native().c_str(), [&]() { changed = true; });

    Sleep(100);
    WriteFile(L"watch.md", "modified");
    WaitForEvent(watcher);
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

    WriteFile(L"stop.md", "modified");
    Sleep(50);
    watcher.CheckForChanges();
    EXPECT_FALSE(changed);
}

// ---- 追加のエッジケース ----

TEST_F(FileLoaderTest, LargeFile)
{
    // 1MBのファイルを作成
    std::string large_content(1024 * 1024, 'A');
    auto path = WriteFile(L"large.md", large_content);
    auto result = FileLoader::LoadFile(path.native().c_str());
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->text.size(), large_content.size());
    EXPECT_EQ(result->byte_size, large_content.size());
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

    // 元のファイルを変更 - ファイル名フィルタでコールバックが発火しないこと
    Sleep(100);
    WriteFile(L"watch1.md", "modified1");
    for (int i = 0; i < 10; i++) {
        WaitForEvent(watcher, 100);
    }
    EXPECT_EQ(change_count, 0);

    // 新しいファイルを変更 - コールバックが発火すること
    Sleep(100);
    WriteFile(L"watch2.md", "modified2");
    WaitForEvent(watcher);
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
    auto result = FileLoader::LoadFile(path.native().c_str());
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->text, "line1\r\nline2\r\nline3");
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

    Sleep(100);
    WriteFile(L"pause.md", "modified1");
    WaitForEvent(watcher);
    ASSERT_EQ(change_count, 1);

    // 変更検出後は一時停止（コールバック抑制、I/Oは継続）
    Sleep(100);
    WriteFile(L"pause.md", "modified2");
    WaitForEvent(watcher, 500);
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

    Sleep(100);
    WriteFile(L"resume.md", "modified1");
    WaitForEvent(watcher);
    ASSERT_EQ(change_count, 1);

    watcher.ResumeWatching();
    EXPECT_NE(watcher.GetEventHandle(), nullptr);

    Sleep(100);
    WriteFile(L"resume.md", "modified2");
    WaitForEvent(watcher);
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
