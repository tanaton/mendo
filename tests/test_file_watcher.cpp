#include <gtest/gtest.h>
#include "file_watcher.h"
#include "test_helpers.h"

class FileWatcherTest : public TempDirTestBase {
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

TEST_F(FileWatcherTest, WatcherDetectsChange)
{
    auto path = WriteTempFile(L"watch.md", "original");

    FileWatcher watcher;
    bool changed = false;
    watcher.StartWatching(OpenWatchTarget(path.native()), [&]() { changed = true; });

    ModifyAndWait(watcher, L"watch.md", "modified");
    EXPECT_TRUE(changed);
}

TEST_F(FileWatcherTest, WatcherDoesNotFireWithoutChange)
{
    auto path = WriteTempFile(L"nochange.md", "content");

    FileWatcher watcher;
    bool changed = false;
    watcher.StartWatching(OpenWatchTarget(path.native()), [&]() { changed = true; });

    // 少し待ってからチェック（変更なしなので発火しないはず）
    Sleep(100);
    watcher.CheckForChanges();
    EXPECT_FALSE(changed);
}

TEST_F(FileWatcherTest, StopWatchingPreventsCallback)
{
    auto path = WriteTempFile(L"stop.md", "content");

    FileWatcher watcher;
    bool changed = false;
    watcher.StartWatching(OpenWatchTarget(path.native()), [&]() { changed = true; });
    watcher.StopWatching();

    WriteTempFile(L"stop.md", "modified");
    Sleep(50);
    watcher.CheckForChanges();
    EXPECT_FALSE(changed);
}

TEST_F(FileWatcherTest, WatcherRestartOnNewFile)
{
    auto path1 = WriteTempFile(L"watch1.md", "content1");
    auto path2 = WriteTempFile(L"watch2.md", "content2");

    FileWatcher watcher;
    int old_count = 0;
    int new_count = 0;
    watcher.StartWatching(OpenWatchTarget(path1.native()), [&]() { old_count++; });

    // 別のファイルの監視に切り替え
    watcher.StartWatching(OpenWatchTarget(path2.native()), [&]() { new_count++; });

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

TEST_F(FileWatcherTest, WatcherDestructorDoesNotCrash)
{
    auto path = WriteTempFile(L"destructor.md", "content");
    {
        FileWatcher watcher;
        watcher.StartWatching(OpenWatchTarget(path.native()), []() static {});
        // デストラクタで安全に監視が停止されること
    }
}

// ---- 監視一時停止 / ResumeWatching テスト ----

TEST_F(FileWatcherTest, ResumeWatchingWithoutWatching)
{
    // 監視未開始でも安全に呼べること
    FileWatcher watcher;
    watcher.ResumeWatching();
}

TEST_F(FileWatcherTest, WatchPausedAfterChangeDetected)
{
    auto path = WriteTempFile(L"pause.md", "original");

    FileWatcher watcher;
    int change_count = 0;
    watcher.StartWatching(OpenWatchTarget(path.native()), [&]() { change_count++; });

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
TEST_F(FileWatcherTest, DiscardPendingChangeSkipsRefireOnResume)
{
    auto path = WriteTempFile(L"discard.md", "original");

    FileWatcher watcher;
    int change_count = 0;
    watcher.StartWatching(OpenWatchTarget(path.native()), [&]() { change_count++; });

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
TEST_F(FileWatcherTest, RequeueIfPausedRefiresOnResume)
{
    auto path = WriteTempFile(L"requeue.md", "original");

    FileWatcher watcher;
    int change_count = 0;
    watcher.StartWatching(OpenWatchTarget(path.native()), [&]() { change_count++; });

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

TEST_F(FileWatcherTest, ResumeWatchingReenablesDetection)
{
    auto path = WriteTempFile(L"resume.md", "original");

    FileWatcher watcher;
    int change_count = 0;
    watcher.StartWatching(OpenWatchTarget(path.native()), [&]() { change_count++; });

    ModifyAndWait(watcher, L"resume.md", "modified1");
    ASSERT_EQ(change_count, 1);

    watcher.ResumeWatching();
    EXPECT_NE(watcher.GetEventHandle(), nullptr);

    ModifyAndWait(watcher, L"resume.md", "modified2");
    EXPECT_EQ(change_count, 2);
}

// ---- GetEventHandle テスト ----

TEST_F(FileWatcherTest, GetEventHandleNullWhenNotWatching)
{
    FileWatcher watcher;
    EXPECT_EQ(watcher.GetEventHandle(), nullptr);
}

TEST_F(FileWatcherTest, GetEventHandleValidWhileWatching)
{
    auto path = WriteTempFile(L"evthandle.md", "content");
    FileWatcher watcher;
    watcher.StartWatching(OpenWatchTarget(path.native()), []() static {});
    EXPECT_NE(watcher.GetEventHandle(), nullptr);
}

TEST_F(FileWatcherTest, GetEventHandleNullAfterStopWatching)
{
    auto path = WriteTempFile(L"evtstop.md", "content");
    FileWatcher watcher;
    watcher.StartWatching(OpenWatchTarget(path.native()), []() static {});
    EXPECT_NE(watcher.GetEventHandle(), nullptr);
    watcher.StopWatching();
    EXPECT_EQ(watcher.GetEventHandle(), nullptr);
}

// 親フォルダを開けなかった (worker の OpenWatchTarget が失敗した) ときは監視しない。今の監視もやめる。
TEST_F(FileWatcherTest, StartWatchingWithoutFolderStopsWatching)
{
    auto path = WriteTempFile(L"nofolder.md", "content");
    FileWatcher watcher;
    watcher.StartWatching(OpenWatchTarget(path.native()), []() static {});
    ASSERT_NE(watcher.GetEventHandle(), nullptr);

    const std::wstring missing_path(L"C:\\__mendo_no_such_dir__\\a.md");
    auto missing = OpenWatchTarget(missing_path);
    EXPECT_FALSE(missing.dir);
    EXPECT_EQ(missing.filename, L"a.md");
    watcher.StartWatching(std::move(missing), []() static {});
    EXPECT_EQ(watcher.GetEventHandle(), nullptr);
}
