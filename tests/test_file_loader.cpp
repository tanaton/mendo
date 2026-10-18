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

    // 少し置いてから name を書き換え、変更通知を待つ。通知が届いたら true。
    bool ModifyAndWait(FileWatcher& watcher, std::wstring_view name, std::string_view content, int timeout_ms = 2000)
    {
        Sleep(100);
        WriteTempFile(name, content);
        return WaitForEvent(watcher, timeout_ms);
    }
};

// BOM を除き改行を LF に揃える。byte_size は BOM 込み・正規化前のディスク上サイズのまま
// (部分書き込みの検出に使うため)。
TEST_F(FileLoaderTest, LoadsTextWithoutBomAndWithLfNewlines)
{
    struct Case {
        const char* label;
        std::string_view content;
        std::string_view text;
    };
    constexpr Case kCases[] = {
        { "Ascii", "Hello, World!", "Hello, World!" },
        { "Multiline", "line1\nline2\nline3", "line1\nline2\nline3" },
        { "CrlfAndCr", "line1\r\nline2\rline3\r\n", "line1\nline2\nline3\n" },
        { "Bom", "\xEF\xBB\xBF" "Hello", "Hello" },
        { "BomOnly", "\xEF\xBB\xBF", "" },
        { "Empty", "", "" },
        { "Japanese", "日本語テスト", "日本語テスト" },
    };
    for (const auto& c : kCases) {
        SCOPED_TRACE(c.label);
        const auto path = WriteTempFile(L"test.md", c.content);
        const auto result = FileLoader::LoadFile(path.native().c_str());
        ASSERT_TRUE(result.has_value());
        EXPECT_EQ(result->text, c.text);
        EXPECT_EQ(result->byte_size, c.content.size());
    }
}

TEST_F(FileLoaderTest, NonExistentFileReturnsError)
{
    auto result = FileLoader::LoadFile(L"C:\\nonexistent_file_12345.md");
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error(), FileLoadError::NotFound);
}

// ---- ファイル監視テスト ----

TEST_F(FileLoaderTest, WatcherDetectsChange)
{
    auto path = WriteTempFile(L"watch.md", "original");

    FileWatcher watcher;
    bool changed = false;
    watcher.StartWatching(path.native().c_str(), [&]() { changed = true; });

    ModifyAndWait(watcher, L"watch.md", "modified");
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
    ASSERT_TRUE(ModifyAndWait(watcher, L"watch1.md", "modified1")) << "watch1.md の変更通知が届かなかった";
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

    ModifyAndWait(watcher, L"pause.md", "modified1");
    ASSERT_EQ(change_count, 1);

    // 変更検出後は一時停止（コールバック抑制、I/Oは継続）
    ModifyAndWait(watcher, L"pause.md", "modified2", 500);
    EXPECT_EQ(change_count, 1);

    // ResumeWatching で蓄積された変更が通知される
    watcher.ResumeWatching();
    EXPECT_EQ(change_count, 2);
}

// 読み直しの直前に捨てた通知は、Resume で再発火しない (同じ保存を二度読まない)。
TEST_F(FileLoaderTest, DiscardPendingChangeSkipsRefireOnResume)
{
    auto path = WriteTempFile(L"discard.md", "original");

    FileWatcher watcher;
    int change_count = 0;
    watcher.StartWatching(path.native().c_str(), [&]() { change_count++; });

    ModifyAndWait(watcher, L"discard.md", "modified1");
    ASSERT_EQ(change_count, 1);

    // 通知が届いていないと、捨てる処理が無くても再発火しないので検証にならない。
    ASSERT_TRUE(ModifyAndWait(watcher, L"discard.md", "modified2"));
    watcher.DiscardPendingChange();
    watcher.ResumeWatching();
    EXPECT_EQ(change_count, 1);
}

// 別ファイルのロード中に届いた表示中ファイルの変更は、読まずに取りやめても Resume で通知し直す。
// 取りこぼすと、別ファイルのロード失敗後に表示中ファイルの保存が反映されないまま監視が止まる。
TEST_F(FileLoaderTest, RequeueIfPausedRefiresOnResume)
{
    auto path = WriteTempFile(L"requeue.md", "original");

    FileWatcher watcher;
    int change_count = 0;
    watcher.StartWatching(path.native().c_str(), [&]() { change_count++; });

    ModifyAndWait(watcher, L"requeue.md", "modified1");
    ASSERT_EQ(change_count, 1);

    watcher.RequeueIfPaused();
    watcher.ResumeWatching();
    EXPECT_EQ(change_count, 2);

    // 一時停止していなければ積み直さない (Resume のたびに空の通知を出さない)。
    watcher.ResumeWatching();
    ASSERT_EQ(change_count, 2);
    watcher.RequeueIfPaused();
    watcher.ResumeWatching();
    EXPECT_EQ(change_count, 2);
}

TEST_F(FileLoaderTest, ResumeWatchingReenablesDetection)
{
    auto path = WriteTempFile(L"resume.md", "original");

    FileWatcher watcher;
    int change_count = 0;
    watcher.StartWatching(path.native().c_str(), [&]() { change_count++; });

    ModifyAndWait(watcher, L"resume.md", "modified1");
    ASSERT_EQ(change_count, 1);

    watcher.ResumeWatching();
    EXPECT_NE(watcher.GetEventHandle(), nullptr);

    ModifyAndWait(watcher, L"resume.md", "modified2");
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
