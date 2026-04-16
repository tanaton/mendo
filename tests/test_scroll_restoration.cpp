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
// ナビゲーション復帰ではアンカー補償に任せるため、Appは設定しない。
TEST(ScrollRestoration, ConsumeNavScrollDoesNotAffectPendingRestoreScrollY)
{
    ScrollRestoration sr;
    sr.pending_nav_scroll_y = 500.3f;
    sr.pending_restore_scroll_y = 1200;

    sr.ConsumeNavScroll();
    EXPECT_EQ(sr.pending_restore_scroll_y, 1200);
}

// ScrollRestoration APIテスト:
// ConsumeNavScroll後にpending_restore_scroll_yを手動設定できることを確認。
// 注: ナビゲーション復帰時はAppが設定しない（アンカー補償に委ねる）。
// セッション復元時のみ SetNodeRestore 経由で設定される。
TEST(ScrollRestoration, NavRestoreThenSetPendingScrollY)
{
    ScrollRestoration sr;
    sr.pending_nav_scroll_y = 500.3f;

    const float scroll_y = sr.ConsumeNavScroll();
    EXPECT_FLOAT_EQ(scroll_y, 500.3f);
    EXPECT_FALSE(sr.HasNavScroll());

    // API上は手動設定可能（セッション復元で使用）
    sr.pending_restore_scroll_y = scroll_y;
    EXPECT_FLOAT_EQ(sr.pending_restore_scroll_y, 500.3f);
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
