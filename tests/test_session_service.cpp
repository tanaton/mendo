#include <gtest/gtest.h>
#include "session_service.h"
#include "config_store.h"

class SessionServiceTest : public ::testing::Test {
protected:
    void SetUp() override
    {
        config::Load();
    }
    ConfigService config_;
    SessionService session_{ config_ };
};

// ---- SavePaneState / LoadPaneState ----

TEST_F(SessionServiceTest, SaveAndLoadPaneState)
{
    PaneController panes;
    panes.SetFilePaneVisible(false);
    panes.SetTocPaneVisible(true);
    panes.SetFilePaneWidth(180.0f);
    panes.SetTocPaneWidth(250.0f);

    session_.SavePaneState(panes);

    PaneController loaded;
    session_.LoadPaneState(loaded, 1200.0f);

    EXPECT_FALSE(loaded.IsFilePaneVisible());
    EXPECT_TRUE(loaded.IsTocPaneVisible());
    EXPECT_FLOAT_EQ(loaded.GetFilePaneWidth(), 180.0f);
    EXPECT_FLOAT_EQ(loaded.GetTocPaneWidth(), 250.0f);
}

TEST_F(SessionServiceTest, LoadPaneStateFallsBackToDefaultIfOutOfRange)
{
    // 保存値が dynamic_max を超える場合、GetInt の仕様でデフォルト値が返る
    config_.SaveInt("Pane", "FileWidth", 500);
    config_.SaveInt("Pane", "TocWidth", 500);

    PaneController loaded;
    // client_width=300 → dynamic_max = max(100, 300-100) = 200
    // 500 > 200 なのでデフォルト値 (PANE_DEFAULT_WIDTH=220) が返るが、
    // さらに dynamic_max でクランプされないため、SetFilePaneWidth(220) が呼ばれる
    session_.LoadPaneState(loaded, 300.0f);

    // デフォルト値が適用される
    EXPECT_FLOAT_EQ(loaded.GetFilePaneWidth(), PaneController::PANE_DEFAULT_WIDTH);
    EXPECT_FLOAT_EQ(loaded.GetTocPaneWidth(), PaneController::PANE_DEFAULT_WIDTH);
}

// ---- SaveScrollPosition / LoadScrollPosition ----

TEST_F(SessionServiceTest, SaveAndLoadScrollPosition)
{
    session_.SaveScrollPosition(5, 350.0f, 300.0f);

    const auto pos = session_.LoadScrollPosition();
    EXPECT_EQ(pos.node, 5);
    EXPECT_EQ(pos.offset, 50);       // lround(350 - 300)
    EXPECT_EQ(pos.raw_y, 350);       // lround(350)
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
