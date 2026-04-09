#include <gtest/gtest.h>
#include <cmath>
#include "scroll_restoration.h"

TEST(ScrollRestoration, InitialState)
{
    ScrollRestoration sr;
    EXPECT_FALSE(sr.HasNavScroll());
    EXPECT_FALSE(sr.HasNodeRestore());
    EXPECT_FLOAT_EQ(sr.pending_nav_scroll_y, -1.0f);
    EXPECT_EQ(sr.pending_restore_node, -1);
    EXPECT_EQ(sr.pending_restore_offset, 0);
    EXPECT_EQ(sr.pending_restore_scroll_y, -1);
}

TEST(ScrollRestoration, HasNavScroll)
{
    ScrollRestoration sr;
    EXPECT_FALSE(sr.HasNavScroll());

    sr.pending_nav_scroll_y = 0.0f;
    EXPECT_TRUE(sr.HasNavScroll());

    sr.pending_nav_scroll_y = 500.0f;
    EXPECT_TRUE(sr.HasNavScroll());
}

TEST(ScrollRestoration, ConsumeNavScroll)
{
    ScrollRestoration sr;
    sr.pending_nav_scroll_y = 250.0f;
    EXPECT_TRUE(sr.HasNavScroll());

    const float v = sr.ConsumeNavScroll();
    EXPECT_FLOAT_EQ(v, 250.0f);
    EXPECT_FALSE(sr.HasNavScroll());
    EXPECT_FLOAT_EQ(sr.pending_nav_scroll_y, -1.0f);
}

TEST(ScrollRestoration, HasNodeRestore)
{
    ScrollRestoration sr;
    EXPECT_FALSE(sr.HasNodeRestore());

    sr.pending_restore_node = 0;
    EXPECT_TRUE(sr.HasNodeRestore());

    sr.pending_restore_node = 42;
    EXPECT_TRUE(sr.HasNodeRestore());
}

TEST(ScrollRestoration, SetNodeRestore)
{
    ScrollRestoration sr;
    sr.SetNodeRestore(10, 50);
    EXPECT_EQ(sr.pending_restore_node, 10);
    EXPECT_EQ(sr.pending_restore_offset, 50);
    EXPECT_EQ(sr.pending_restore_scroll_y, -1);
}

TEST(ScrollRestoration, SetNodeRestoreWithScrollY)
{
    ScrollRestoration sr;
    sr.SetNodeRestore(5, 30, 1200);
    EXPECT_EQ(sr.pending_restore_node, 5);
    EXPECT_EQ(sr.pending_restore_offset, 30);
    EXPECT_EQ(sr.pending_restore_scroll_y, 1200);
}

TEST(ScrollRestoration, ClearNodeRestore)
{
    ScrollRestoration sr;
    sr.SetNodeRestore(10, 50, 800);
    sr.ClearNodeRestore();
    EXPECT_FALSE(sr.HasNodeRestore());
    EXPECT_EQ(sr.pending_restore_node, -1);
    EXPECT_EQ(sr.pending_restore_offset, 0);
    // ClearNodeRestoreはpending_restore_scroll_yを保持する
    // （遅延レイアウト完了後の最終補正に必要なため）
    EXPECT_EQ(sr.pending_restore_scroll_y, 800);
}

// ConsumeNavScrollはpending_restore_scroll_yに影響しない。
// 呼び出し側（App）がナビゲーション復元時に別途設定する責務を持つ。
TEST(ScrollRestoration, ConsumeNavScrollDoesNotAffectPendingRestoreScrollY)
{
    ScrollRestoration sr;
    sr.pending_nav_scroll_y = 500.3f;
    sr.pending_restore_scroll_y = 1200;

    sr.ConsumeNavScroll();
    EXPECT_EQ(sr.pending_restore_scroll_y, 1200);
}

// ナビゲーション復元のシナリオ:
// ConsumeNavScroll後にpending_restore_scroll_yを設定し、
// 遅延レイアウト完了時のドリフト補正に使う。
TEST(ScrollRestoration, NavRestoreThenSetPendingScrollY)
{
    ScrollRestoration sr;
    sr.pending_nav_scroll_y = 500.3f;

    const float scroll_y = sr.ConsumeNavScroll();
    EXPECT_FLOAT_EQ(scroll_y, 500.3f);
    EXPECT_FALSE(sr.HasNavScroll());

    // App側が遅延レイアウトのドリフト補正用に設定する
    sr.pending_restore_scroll_y = static_cast<int>(std::lround(scroll_y));
    EXPECT_EQ(sr.pending_restore_scroll_y, 500);
}

// 新規ファイルオープン時に前回ナビゲーションの残留値をクリアするシナリオ
TEST(ScrollRestoration, StaleNavValueClearedOnFreshOpen)
{
    ScrollRestoration sr;

    // ナビゲーション復元でpending_restore_scroll_yが設定された状態
    sr.pending_restore_scroll_y = 500;

    // 新規ファイルオープン時にクリア
    sr.pending_restore_scroll_y = -1;
    EXPECT_EQ(sr.pending_restore_scroll_y, -1);
}

TEST(ScrollRestoration, Reset)
{
    ScrollRestoration sr;
    sr.pending_nav_scroll_y = 100.0f;
    sr.SetNodeRestore(10, 50, 800);

    sr.Reset();
    EXPECT_FALSE(sr.HasNavScroll());
    EXPECT_FALSE(sr.HasNodeRestore());
    EXPECT_FLOAT_EQ(sr.pending_nav_scroll_y, -1.0f);
    EXPECT_EQ(sr.pending_restore_node, -1);
    EXPECT_EQ(sr.pending_restore_offset, 0);
    EXPECT_EQ(sr.pending_restore_scroll_y, -1);
}
