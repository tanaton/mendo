#include <gtest/gtest.h>
#include "config_service.h"
#include "pane_layout.h"
#include "test_helpers.h"

class SessionTest : public TempDirTestBase {
protected:
    ConfigService config_;

    PaneController LoadPanes() const
    {
        PaneController panes;
        session::LoadPanes(config_, panes);
        return panes;
    }
};

// ---- SavePanes / LoadPanes ----

TEST_F(SessionTest, SaveAndLoadPanes)
{
    PaneController saved;
    saved.SetSidePaneVisible(PaneTarget::File, false);
    saved.SetSidePaneWidth(PaneTarget::File, 180.0f);
    saved.SetSidePaneWidth(PaneTarget::Toc, 250.0f);
    session::SavePanes(config_, saved);

    const auto loaded = LoadPanes();

    EXPECT_FALSE(loaded.IsSidePaneVisible(PaneTarget::File));
    EXPECT_TRUE(loaded.IsSidePaneVisible(PaneTarget::Toc));
    EXPECT_FLOAT_EQ(loaded.GetSidePaneWidth(PaneTarget::File), 180.0f);
    EXPECT_FLOAT_EQ(loaded.GetSidePaneWidth(PaneTarget::Toc), 250.0f);
}

// 読み込み先の今の状態は残さない。
TEST_F(SessionTest, LoadPanesOverwritesCurrentState)
{
    PaneController saved;
    saved.SetSidePaneVisible(PaneTarget::Toc, false);
    session::SavePanes(config_, saved);

    PaneController panes;
    panes.SetSidePaneVisible(PaneTarget::File, false);
    panes.SetSidePaneWidth(PaneTarget::File, 400.0f);
    session::LoadPanes(config_, panes);

    EXPECT_TRUE(panes.IsSidePaneVisible(PaneTarget::File));
    EXPECT_FALSE(panes.IsSidePaneVisible(PaneTarget::Toc));
    EXPECT_FLOAT_EQ(panes.GetSidePaneWidth(PaneTarget::File), SIDE_PANE_DEFAULT_WIDTH);
}

TEST_F(SessionTest, LoadPanesKeepsSavedWidthWithoutWindowClamp)
{
    // 過剰幅の制限はレイアウト側 (md_width = max(md_min_width, total_width - x) で MD ペイン
    // 下限を保証) に委ねるため、LoadPanes はウィンドウ幅に基づく上限 clamp を行わず、
    // 保存値をそのまま使う。これにより狭いウィンドウ起動時に保存値が min へ潰れて
    // 終了時の再保存で恒久喪失するのを防ぐ。
    config_.SaveInt("Pane", "FileWidth", 500);
    config_.SaveInt("Pane", "TocWidth", 500);

    const auto loaded = LoadPanes();

    EXPECT_FLOAT_EQ(loaded.GetSidePaneWidth(PaneTarget::File), 500.0f);
    EXPECT_FLOAT_EQ(loaded.GetSidePaneWidth(PaneTarget::Toc), 500.0f);
}

TEST_F(SessionTest, LoadPanesUsesDefaultWhenNothingSaved)
{
    const auto loaded = LoadPanes();

    EXPECT_TRUE(loaded.IsSidePaneVisible(PaneTarget::File));
    EXPECT_TRUE(loaded.IsSidePaneVisible(PaneTarget::Toc));
    EXPECT_FLOAT_EQ(loaded.GetSidePaneWidth(PaneTarget::File), SIDE_PANE_DEFAULT_WIDTH);
    EXPECT_FLOAT_EQ(loaded.GetSidePaneWidth(PaneTarget::Toc), SIDE_PANE_DEFAULT_WIDTH);
}

// 幅はズームを含まない DIP 値として往復する。読み側は倍率を掛けない。
TEST_F(SessionTest, SavedWidthRoundTripsAsDip)
{
    PaneController saved;
    saved.SetSidePaneWidth(PaneTarget::File, 300.0f);
    saved.SetSidePaneWidth(PaneTarget::Toc, 260.0f);
    saved.SidePaneZoom(PaneTarget::File).SetIndex(ZOOM_DEFAULT_INDEX + 2);
    session::SavePanes(config_, saved);

    const auto loaded = LoadPanes();

    EXPECT_FLOAT_EQ(loaded.GetSidePaneWidth(PaneTarget::File), 300.0f);
    EXPECT_FLOAT_EQ(loaded.GetSidePaneWidth(PaneTarget::Toc), 260.0f);
}

// ペインの倍率はペインごとに往復する。
TEST_F(SessionTest, PaneZoomRoundTrips)
{
    PaneController saved;
    saved.SidePaneZoom(PaneTarget::File).SetIndex(ZOOM_DEFAULT_INDEX + 2);
    saved.SidePaneZoom(PaneTarget::Toc).SetIndex(ZOOM_DEFAULT_INDEX - 1);
    session::SavePanes(config_, saved);

    const auto loaded = LoadPanes();

    EXPECT_EQ(loaded.SidePaneZoom(PaneTarget::File).Index(), ZOOM_DEFAULT_INDEX + 2);
    EXPECT_EQ(loaded.SidePaneZoom(PaneTarget::Toc).Index(), ZOOM_DEFAULT_INDEX - 1);
}

// 保存が無ければ等倍。
TEST_F(SessionTest, PaneZoomDefaultsToActualSize)
{
    const auto loaded = LoadPanes();

    EXPECT_TRUE(loaded.SidePaneZoom(PaneTarget::File).IsDefault());
    EXPECT_TRUE(loaded.SidePaneZoom(PaneTarget::Toc).IsDefault());
}

// ---- SaveScrollPosition / LoadScrollPosition ----

TEST_F(SessionTest, SaveAndLoadScrollPosition)
{
    session::SaveScrollPosition(config_, 5, 49.6f);

    const auto pos = session::LoadScrollPosition(config_);
    EXPECT_EQ(pos.node, 5);
    EXPECT_EQ(pos.offset, 50); // lround(49.6)
}

// ---- SaveLastFilePath / LoadLastFilePath ----

TEST_F(SessionTest, SaveLastFilePathStoresPath)
{
    // LoadLastFilePath は実在しないパスを弾くため、実ファイルを作って往復させる。
    const auto file = WriteTempFile(L"last.md", "# last").wstring();
    session::SaveLastFilePath(config_, file);
    EXPECT_EQ(session::LoadLastFilePath(config_), std::wstring_view{ file });
}

TEST_F(SessionTest, SaveLastFilePathSkipsEmpty)
{
    // 空パスで既存の保存値を上書きしない。
    const auto file = WriteTempFile(L"last.md", "# last").wstring();
    session::SaveLastFilePath(config_, file);
    session::SaveLastFilePath(config_, L"");
    EXPECT_EQ(session::LoadLastFilePath(config_), std::wstring_view{ file });
}

// UNC (\\host)・拡張パス (\\?\)・デバイスパス (\\.\) と、存在しないファイルは返さない。
TEST_F(SessionTest, LoadLastFilePathRejectsRemoteDeviceAndMissingPaths)
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
        EXPECT_TRUE(session::LoadLastFilePath(config_).empty());
    }
}

// 実在するファイルを指すデバイスパスも弾く。存在しないパスでは存在確認で空になり、判定を外しても通ってしまう。
TEST_F(SessionTest, LoadLastFilePathRejectsDeviceFormsOfExistingFile)
{
    const auto file = WriteTempFile(L"last.md", "# last").wstring();
    for (const std::wstring& prefixed : { L"\\\\?\\" + file, L"\\\\.\\" + file, L"//./" + file, L"\\??\\" + file }) {
        SCOPED_TRACE(::testing::PrintToString(prefixed));
        ASSERT_NE(::GetFileAttributesW(prefixed.c_str()), INVALID_FILE_ATTRIBUTES) << "前提: このパスでファイルを開けること";
        config_.SaveWString("Session", "LastFile", prefixed);
        EXPECT_TRUE(session::LoadLastFilePath(config_).empty());
    }
}

TEST_F(SessionTest, LoadLastFilePathReturnsEmptyWhenNothingSaved)
{
    auto path = session::LoadLastFilePath(config_);
    EXPECT_TRUE(path.empty());
}

// ---- SaveScrollPosition / LoadScrollPosition の境界 ----

TEST_F(SessionTest, SaveScrollPosition_NegativeOffsetRoundtrips)
{
    // ノード先頭より上を見ている (scroll_y < node_y) と offset は負になる。
    session::SaveScrollPosition(config_, 3, -150.0f);
    const auto pos = session::LoadScrollPosition(config_);
    EXPECT_EQ(pos.node, 3);
    EXPECT_EQ(pos.offset, -150);
}

TEST_F(SessionTest, LoadScrollPosition_DefaultsWhenNothingSaved)
{
    const auto pos = session::LoadScrollPosition(config_);
    EXPECT_EQ(pos.node, -1);
    EXPECT_EQ(pos.offset, 0);
}

// node は [-1, 1000000]、offset は ±1000000 を外れると既定値 (-1 / 0) に倒す。
TEST_F(SessionTest, LoadScrollPosition_OutOfRangeFallsBackToDefault)
{
    config_.SaveInt("Session", "ScrollNode", 10000000);
    config_.SaveInt("Session", "ScrollOffset", 99999999);
    const auto pos = session::LoadScrollPosition(config_);
    EXPECT_EQ(pos.node, -1);
    EXPECT_EQ(pos.offset, 0);
}
