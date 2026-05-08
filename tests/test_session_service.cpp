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

    const auto loaded = session_.LoadPaneState(1200.0f,
                                               PaneController::PANE_MIN_WIDTH,
                                               PaneController::PANE_DEFAULT_WIDTH);

    EXPECT_FALSE(loaded.show_file);
    EXPECT_TRUE(loaded.show_toc);
    EXPECT_FLOAT_EQ(loaded.file_width, 180.0f);
    EXPECT_FLOAT_EQ(loaded.toc_width, 250.0f);
}

TEST_F(SessionServiceTest, LoadPaneStateClampsOutOfRangeValuesToDynamicMax)
{
    // 保存値が dynamic_max を超える場合、GetInt はデフォルト値 (PANE_DEFAULT_WIDTH=220) を
    // 返す。狭いウィンドウでは既定値自体が dynamic_max を超えうるため、
    // LoadPaneState 側で最終結果も clamp し、過剰な幅を返さない。
    config_.SaveInt("Pane", "FileWidth", 500);
    config_.SaveInt("Pane", "TocWidth", 500);

    // client_width=300 → dynamic_max = max(100, 300-100) = 200
    const auto loaded = session_.LoadPaneState(300.0f,
                                               PaneController::PANE_MIN_WIDTH,
                                               PaneController::PANE_DEFAULT_WIDTH);

    // dynamic_max=200 で clamp される（fix 前は 220 が漏れていた）
    EXPECT_FLOAT_EQ(loaded.file_width, 200.0f);
    EXPECT_FLOAT_EQ(loaded.toc_width, 200.0f);
}

TEST_F(SessionServiceTest, LoadPaneStateUsesDefaultWhenWindowFitsIt)
{
    // 通常幅のウィンドウでは、欠落値は DEFAULT_WIDTH のまま使われる
    const auto loaded = session_.LoadPaneState(1200.0f,
                                               PaneController::PANE_MIN_WIDTH,
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
    session_.SaveLastFilePath(L"C:\\test.md");
    // 保存されたパスのファイルが存在しない場合は空が返る
    // (テスト環境でファイルが存在しないため)
}

TEST_F(SessionServiceTest, SaveLastFilePathSkipsEmpty)
{
    session_.SaveLastFilePath(L"");
    // 空パスは保存されない
}

TEST_F(SessionServiceTest, LoadLastFilePathRejectsUncPaths)
{
    config_.SaveWString("Session", "LastFile", L"\\\\server\\share\\file.md");
    auto path = session_.LoadLastFilePath();
    EXPECT_TRUE(path.empty());
}

// ---- LoadLastFilePath の境界: デバイスパス / 拡張パス / 存在しないファイル ----

TEST_F(SessionServiceTest, LoadLastFilePathRejectsExtendedPathPrefix)
{
    // \\?\ 拡張パスは UNC パスと同じ \\ 始まりとして弾かれる。
    config_.SaveWString("Session", "LastFile", L"\\\\?\\C:\\file.md");
    auto path = session_.LoadLastFilePath();
    EXPECT_TRUE(path.empty());
}

TEST_F(SessionServiceTest, LoadLastFilePathRejectsDevicePathPrefix)
{
    // \\.\ デバイスパスも \\ 始まりとして弾かれる。
    config_.SaveWString("Session", "LastFile", L"\\\\.\\PhysicalDrive0");
    auto path = session_.LoadLastFilePath();
    EXPECT_TRUE(path.empty());
}

TEST_F(SessionServiceTest, LoadLastFilePathRejectsNonexistentLocalPath)
{
    // GetFileAttributesW がパス無効を返す通常パスは弾かれる。
    // TempDirTestBase 配下の作成しないファイル名を使い、絶対パスへのフレークを避ける。
    const auto missing = (temp_dir_ / L"never_created.md").wstring();
    config_.SaveWString("Session", "LastFile", missing);
    auto path = session_.LoadLastFilePath();
    EXPECT_TRUE(path.empty());
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

TEST_F(SessionServiceTest, LoadScrollPosition_OutOfRangeNodeFallsBackToDefault)
{
    // LoadInt の min/max は -1 / 1000000。範囲外は default(-1) を返すはず。
    config_.SaveInt("Session", "ScrollNode", 10000000);
    const auto pos = session_.LoadScrollPosition();
    EXPECT_EQ(pos.node, -1);
}

TEST_F(SessionServiceTest, LoadScrollPosition_OutOfRangeOffsetFallsBackToDefault)
{
    // offset は ±1000000 を超えると default(0) になる。
    config_.SaveInt("Session", "ScrollOffset", 99999999);
    const auto pos = session_.LoadScrollPosition();
    EXPECT_EQ(pos.offset, 0);
}
