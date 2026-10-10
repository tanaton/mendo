#include <gtest/gtest.h>
#include "file_loader.h"
#include "file_watcher.h"
#include "i18n.h"
#include "test_helpers.h"

class FileLoaderTest : public TempDirTestBase {
protected:
    // イベントハンドルで変更通知を待ってから CheckForChanges する。通知が届いたら true。
    bool WaitForEvent(FileWatcher& watcher, int timeout_ms = 2000)
    {
        HANDLE h = watcher.GetEventHandle();
        const bool signaled = h && WaitForSingleObject(h, static_cast<DWORD>(timeout_ms)) == WAIT_OBJECT_0;
        watcher.CheckForChanges();
        return signaled;
    }
};

TEST_F(FileLoaderTest, LoadsUtf8File)
{
    auto path = WriteTempFile(L"test.md", "Hello, World!");
    auto result = FileLoader::LoadFile(path.native().c_str());
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->text, "Hello, World!");
    EXPECT_EQ(result->byte_size, 13u);
}

TEST_F(FileLoaderTest, LoadsMultilineFile)
{
    auto path = WriteTempFile(L"multi.md", "line1\nline2\nline3");
    auto result = FileLoader::LoadFile(path.native().c_str());
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->text, "line1\nline2\nline3");
}

// byte_size は正規化前のディスク上サイズのまま (部分書き込み検出で使うため)。
TEST_F(FileLoaderTest, NormalizesNewlinesToLf)
{
    auto path = WriteTempFile(L"crlf.md", "line1\r\nline2\rline3\r\n");
    auto result = FileLoader::LoadFile(path.native().c_str());
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->text, "line1\nline2\nline3\n");
    EXPECT_EQ(result->byte_size, 20u);
}

// byte_size は BOM 含む元サイズ。
TEST_F(FileLoaderTest, StripsUtf8Bom)
{
    auto path = WriteTempFile(L"bom.md", "\xEF\xBB\xBF" "Hello");
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
    auto path = WriteTempFile(L"empty.md", "");
    auto result = FileLoader::LoadFile(path.native().c_str());
    ASSERT_TRUE(result.has_value());
    EXPECT_TRUE(result->text.empty());
    EXPECT_EQ(result->byte_size, 0u);
}

TEST_F(FileLoaderTest, LoadsJapaneseUtf8)
{
    auto path = WriteTempFile(L"jp.md", "日本語テスト");
    auto result = FileLoader::LoadFile(path.native().c_str());
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->text, "日本語テスト");
}

TEST_F(FileLoaderTest, BomOnlyFileReturnsEmpty)
{
    auto path = WriteTempFile(L"bomonly.md", "\xEF\xBB\xBF");
    auto result = FileLoader::LoadFile(path.native().c_str());
    ASSERT_TRUE(result.has_value());
    EXPECT_TRUE(result->text.empty());
    EXPECT_EQ(result->byte_size, 3u);
}

// ---- ファイル監視テスト ----

TEST_F(FileLoaderTest, WatcherDetectsChange)
{
    auto path = WriteTempFile(L"watch.md", "original");

    FileWatcher watcher;
    bool changed = false;
    watcher.StartWatching(path.native().c_str(), [&]() { changed = true; });

    Sleep(100);
    WriteTempFile(L"watch.md", "modified");
    WaitForEvent(watcher);
    EXPECT_TRUE(changed);
}

TEST_F(FileLoaderTest, WatcherDoesNotFireWithoutChange)
{
    auto path = WriteTempFile(L"nochange.md", "content");

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
    auto path = WriteTempFile(L"stop.md", "content");

    FileWatcher watcher;
    bool changed = false;
    watcher.StartWatching(path.native().c_str(), [&]() { changed = true; });
    watcher.StopWatching();

    WriteTempFile(L"stop.md", "modified");
    Sleep(50);
    watcher.CheckForChanges();
    EXPECT_FALSE(changed);
}

// ---- 追加のエッジケース ----

TEST_F(FileLoaderTest, LargeFile)
{
    // 1MBのファイルを作成
    std::string large_content(1024 * 1024, 'A');
    auto path = WriteTempFile(L"large.md", large_content);
    auto result = FileLoader::LoadFile(path.native().c_str());
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->text.size(), large_content.size());
    EXPECT_EQ(result->byte_size, large_content.size());
}

TEST_F(FileLoaderTest, WatcherRestartOnNewFile)
{
    auto path1 = WriteTempFile(L"watch1.md", "content1");
    auto path2 = WriteTempFile(L"watch2.md", "content2");

    FileWatcher watcher;
    int old_count = 0;
    int new_count = 0;
    watcher.StartWatching(path1.native().c_str(), [&]() { old_count++; });

    // 別のファイルの監視に切り替え
    watcher.StartWatching(path2.native().c_str(), [&]() { new_count++; });

    // 元のファイルを変更 - ディレクトリ通知は届くが、ファイル名フィルタでコールバックが発火しないこと。
    // 誤って発火すると一時停止に入り後続の通知を握りつぶすため、最終回数ではなくここで検証する。
    Sleep(100);
    WriteTempFile(L"watch1.md", "modified1");
    ASSERT_TRUE(WaitForEvent(watcher)) << "watch1.md の変更通知が届かなかった";
    EXPECT_EQ(old_count, 0);
    EXPECT_EQ(new_count, 0);

    // 新しいファイルを変更 - コールバックが発火すること (watch1 の残りの通知が先に届くことがある)
    WriteTempFile(L"watch2.md", "modified2");
    ASSERT_TRUE(PollUntil([&] {
        watcher.CheckForChanges();
        return new_count > 0;
    }));
    EXPECT_EQ(old_count, 0);
    EXPECT_EQ(new_count, 1);
}

TEST_F(FileLoaderTest, WatcherDestructorDoesNotCrash)
{
    auto path = WriteTempFile(L"destructor.md", "content");
    {
        FileWatcher watcher;
        watcher.StartWatching(path.native().c_str(), []() static {});
        // デストラクタで安全に監視が停止されること
    }
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
    auto path = WriteTempFile(L"pause.md", "original");

    FileWatcher watcher;
    int change_count = 0;
    watcher.StartWatching(path.native().c_str(), [&]() { change_count++; });

    Sleep(100);
    WriteTempFile(L"pause.md", "modified1");
    WaitForEvent(watcher);
    ASSERT_EQ(change_count, 1);

    // 変更検出後は一時停止（コールバック抑制、I/Oは継続）
    Sleep(100);
    WriteTempFile(L"pause.md", "modified2");
    WaitForEvent(watcher, 500);
    EXPECT_EQ(change_count, 1);

    // ResumeWatching で蓄積された変更が通知される
    watcher.ResumeWatching();
    EXPECT_EQ(change_count, 2);
}

TEST_F(FileLoaderTest, ResumeWatchingReenablesDetection)
{
    auto path = WriteTempFile(L"resume.md", "original");

    FileWatcher watcher;
    int change_count = 0;
    watcher.StartWatching(path.native().c_str(), [&]() { change_count++; });

    Sleep(100);
    WriteTempFile(L"resume.md", "modified1");
    WaitForEvent(watcher);
    ASSERT_EQ(change_count, 1);

    watcher.ResumeWatching();
    EXPECT_NE(watcher.GetEventHandle(), nullptr);

    Sleep(100);
    WriteTempFile(L"resume.md", "modified2");
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
    auto path = WriteTempFile(L"evthandle.md", "content");
    FileWatcher watcher;
    watcher.StartWatching(path.native().c_str(), []() static {});
    EXPECT_NE(watcher.GetEventHandle(), nullptr);
}

TEST_F(FileLoaderTest, GetEventHandleNullAfterStopWatching)
{
    auto path = WriteTempFile(L"evtstop.md", "content");
    FileWatcher watcher;
    watcher.StartWatching(path.native().c_str(), []() static {});
    EXPECT_NE(watcher.GetEventHandle(), nullptr);
    watcher.StopWatching();
    EXPECT_EQ(watcher.GetEventHandle(), nullptr);
}

// ---- FileLoadErrorMessage ----

TEST(FileLoadErrorMessageTest, MapsEachErrorToToastText)
{
    const auto& s = i18n::kJa;
    EXPECT_EQ(FileLoadErrorMessage(FileLoadError::NotFound, s), s.toast_file_not_found);
    EXPECT_EQ(FileLoadErrorMessage(FileLoadError::TooLarge, s), s.toast_file_too_large);
    EXPECT_EQ(FileLoadErrorMessage(FileLoadError::ReadFailed, s), s.toast_file_read_failed);
}

// キャンセルはユーザー操作 (別ファイルへの切り替え等) の結果なのでトーストを出さない。
TEST(FileLoadErrorMessageTest, CancelledHasNoMessage)
{
    EXPECT_TRUE(FileLoadErrorMessage(FileLoadError::Cancelled, i18n::kJa).empty());
    EXPECT_TRUE(FileLoadErrorMessage(FileLoadError::Cancelled, i18n::kEn).empty());
}
