#include <gtest/gtest.h>
#include "config_service.h"
#include "pane_controller.h"
#include "test_helpers.h"

class SessionServiceTest : public TempDirTestBase {
protected:
    ConfigService config_;
    SessionService session_{ config_ };
};

// ---- SavePaneState / LoadPaneState ----

TEST_F(SessionServiceTest, SaveAndLoadPaneState)
{
    SessionService::PaneState saved{
        .show_file = false,
        .show_toc = true,
        .file_width = 180.0f,
        .toc_width = 250.0f,
    };
    session_.SavePaneState(saved);

    const auto loaded = session_.LoadPaneState(PaneController::PANE_MIN_WIDTH,
                                               PaneController::PANE_DEFAULT_WIDTH);

    EXPECT_FALSE(loaded.show_file);
    EXPECT_TRUE(loaded.show_toc);
    EXPECT_FLOAT_EQ(loaded.file_width, 180.0f);
    EXPECT_FLOAT_EQ(loaded.toc_width, 250.0f);
}

TEST_F(SessionServiceTest, LoadPaneStateKeepsSavedWidthWithoutWindowClamp)
{
    // 過剰幅の制限はレイアウト側 (md_width = max(md_min_width, total_width - x) で MD ペイン
    // 下限を保証) に委ねるため、LoadPaneState はウィンドウ幅に基づく上限 clamp を行わず、
    // 保存値をそのまま返す。これにより狭いウィンドウ起動時に保存値が min へ潰れて
    // 終了時の再保存で恒久喪失するのを防ぐ。
    config_.SaveInt("Pane", "FileWidth", 500);
    config_.SaveInt("Pane", "TocWidth", 500);

    const auto loaded = session_.LoadPaneState(PaneController::PANE_MIN_WIDTH,
                                               PaneController::PANE_DEFAULT_WIDTH);

    EXPECT_FLOAT_EQ(loaded.file_width, 500.0f);
    EXPECT_FLOAT_EQ(loaded.toc_width, 500.0f);
}

TEST_F(SessionServiceTest, LoadPaneStateUsesDefaultWhenWindowFitsIt)
{
    // 通常幅のウィンドウでは、欠落値は DEFAULT_WIDTH のまま使われる
    const auto loaded = session_.LoadPaneState(PaneController::PANE_MIN_WIDTH,
                                               PaneController::PANE_DEFAULT_WIDTH);

    EXPECT_FLOAT_EQ(loaded.file_width, PaneController::PANE_DEFAULT_WIDTH);
    EXPECT_FLOAT_EQ(loaded.toc_width, PaneController::PANE_DEFAULT_WIDTH);
}

// ---- SaveScrollPosition / LoadScrollPosition ----

TEST_F(SessionServiceTest, SaveAndLoadScrollPosition)
{
    session_.SaveScrollPosition(5, 350.0f, 300.0f);

    const auto pos = session_.LoadScrollPosition();
    EXPECT_EQ(pos.node, 5);
    EXPECT_EQ(pos.offset, 50);       // lround(350 - 300)
}

// ---- SaveLastFilePath / LoadLastFilePath ----

TEST_F(SessionServiceTest, SaveLastFilePathStoresPath)
{
    // LoadLastFilePath は実在しないパスを弾くため、実ファイルを作って往復させる。
    const auto file = WriteTempFile(L"last.md", "# last").wstring();
    session_.SaveLastFilePath(file);
    EXPECT_EQ(session_.LoadLastFilePath(), std::wstring_view{ file });
}

TEST_F(SessionServiceTest, SaveLastFilePathSkipsEmpty)
{
    // 空パスで既存の保存値を上書きしない。
    const auto file = WriteTempFile(L"last.md", "# last").wstring();
    session_.SaveLastFilePath(file);
    session_.SaveLastFilePath(L"");
    EXPECT_EQ(session_.LoadLastFilePath(), std::wstring_view{ file });
}

// UNC (\\host)・拡張パス (\\?\)・デバイスパス (\\.\) と、存在しないファイルは返さない。
TEST_F(SessionServiceTest, LoadLastFilePathRejectsRemoteDeviceAndMissingPaths)
{
    // 存在しないファイルは temp_dir_ 配下の作らない名前にし、絶対パスへのフレークを避ける。
    const std::wstring kPaths[] = {
        L"\\\\server\\share\\file.md",
        L"\\\\?\\C:\\file.md",
        L"\\\\.\\PhysicalDrive0",
        (temp_dir_ / L"never_created.md").wstring(),
    };
    for (const auto& saved : kPaths) {
        SCOPED_TRACE(::testing::PrintToString(saved));
        config_.SaveWString("Session", "LastFile", saved);
        EXPECT_TRUE(session_.LoadLastFilePath().empty());
    }
}

// 実在するファイルを指すデバイスパスも弾く。存在しないパスでは存在確認で空になり、判定を外しても通ってしまう。
TEST_F(SessionServiceTest, LoadLastFilePathRejectsDeviceFormsOfExistingFile)
{
    const auto file = WriteTempFile(L"last.md", "# last").wstring();
    for (const std::wstring& prefixed : { L"\\\\?\\" + file, L"\\\\.\\" + file, L"//./" + file, L"\\??\\" + file }) {
        SCOPED_TRACE(::testing::PrintToString(prefixed));
        ASSERT_NE(::GetFileAttributesW(prefixed.c_str()), INVALID_FILE_ATTRIBUTES) << "前提: このパスでファイルを開けること";
        config_.SaveWString("Session", "LastFile", prefixed);
        EXPECT_TRUE(session_.LoadLastFilePath().empty());
    }
}

TEST_F(SessionServiceTest, LoadLastFilePathReturnsEmptyWhenNothingSaved)
{
    auto path = session_.LoadLastFilePath();
    EXPECT_TRUE(path.empty());
}

// ---- SaveScrollPosition / LoadScrollPosition の境界 ----

TEST_F(SessionServiceTest, SaveScrollPosition_NegativeOffsetRoundtrips)
{
    // 反転スクロール（node_y > scroll_y）で offset が負になるケース。
    session_.SaveScrollPosition(3, 100.0f, 250.0f);
    const auto pos = session_.LoadScrollPosition();
    EXPECT_EQ(pos.node, 3);
    EXPECT_EQ(pos.offset, -150);
}

TEST_F(SessionServiceTest, LoadScrollPosition_DefaultsWhenNothingSaved)
{
    const auto pos = session_.LoadScrollPosition();
    EXPECT_EQ(pos.node, -1);
    EXPECT_EQ(pos.offset, 0);
}

// node は [-1, 1000000]、offset は ±1000000 を外れると既定値 (-1 / 0) に倒す。
TEST_F(SessionServiceTest, LoadScrollPosition_OutOfRangeFallsBackToDefault)
{
    config_.SaveInt("Session", "ScrollNode", 10000000);
    config_.SaveInt("Session", "ScrollOffset", 99999999);
    const auto pos = session_.LoadScrollPosition();
    EXPECT_EQ(pos.node, -1);
    EXPECT_EQ(pos.offset, 0);
}
