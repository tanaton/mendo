#include <gtest/gtest.h>
#include "nav_history.h"

class NavHistoryTest : public ::testing::Test {
protected:
    NavHistory hist_;
};

// ─── Basic state ───

TEST_F(NavHistoryTest, InitiallyEmpty) {
    EXPECT_FALSE(hist_.CanGoBack());
    EXPECT_FALSE(hist_.CanGoForward());
    EXPECT_EQ(hist_.BackSize(), 0u);
    EXPECT_EQ(hist_.ForwardSize(), 0u);
}

TEST_F(NavHistoryTest, GoBackOnEmptyReturnsFalse) {
    NavEntry out;
    EXPECT_FALSE(hist_.GoBack({L"a.md", 0.0f}, out));
}

TEST_F(NavHistoryTest, GoForwardOnEmptyReturnsFalse) {
    NavEntry out;
    EXPECT_FALSE(hist_.GoForward({L"a.md", 0.0f}, out));
}

// ─── Push / GoBack ───

TEST_F(NavHistoryTest, PushThenGoBack) {
    hist_.Push({L"a.md", 100.0f});
    EXPECT_TRUE(hist_.CanGoBack());
    EXPECT_FALSE(hist_.CanGoForward());

    NavEntry out;
    EXPECT_TRUE(hist_.GoBack({L"b.md", 200.0f}, out));
    EXPECT_EQ(out.file_path, L"a.md");
    EXPECT_FLOAT_EQ(out.scroll_y, 100.0f);

    // After going back, forward should be available
    EXPECT_TRUE(hist_.CanGoForward());
    EXPECT_FALSE(hist_.CanGoBack());
}

// ─── GoBack then GoForward ───

TEST_F(NavHistoryTest, GoBackThenGoForward) {
    hist_.Push({L"a.md", 0.0f});

    NavEntry out;
    hist_.GoBack({L"b.md", 50.0f}, out);

    EXPECT_TRUE(hist_.GoForward({L"a.md", 0.0f}, out));
    EXPECT_EQ(out.file_path, L"b.md");
    EXPECT_FLOAT_EQ(out.scroll_y, 50.0f);
}

// ─── New navigation clears forward stack ───

TEST_F(NavHistoryTest, PushClearsForwardStack) {
    hist_.Push({L"a.md", 0.0f});

    NavEntry out;
    hist_.GoBack({L"b.md", 0.0f}, out);
    EXPECT_TRUE(hist_.CanGoForward());

    // New navigation should clear forward
    hist_.Push({L"a.md", 0.0f});
    EXPECT_FALSE(hist_.CanGoForward());
}

// ─── Multiple entries ───

TEST_F(NavHistoryTest, MultipleBackForward) {
    // Simulate: open A -> open B -> open C
    hist_.Push({L"a.md", 10.0f});  // before opening B
    hist_.Push({L"b.md", 20.0f});  // before opening C

    EXPECT_EQ(hist_.BackSize(), 2u);

    NavEntry out;
    // Go back from C to B
    EXPECT_TRUE(hist_.GoBack({L"c.md", 30.0f}, out));
    EXPECT_EQ(out.file_path, L"b.md");
    EXPECT_FLOAT_EQ(out.scroll_y, 20.0f);

    // Go back from B to A
    EXPECT_TRUE(hist_.GoBack({L"b.md", 20.0f}, out));
    EXPECT_EQ(out.file_path, L"a.md");
    EXPECT_FLOAT_EQ(out.scroll_y, 10.0f);

    EXPECT_FALSE(hist_.CanGoBack());
    EXPECT_EQ(hist_.ForwardSize(), 2u);

    // Go forward from A to B
    EXPECT_TRUE(hist_.GoForward({L"a.md", 10.0f}, out));
    EXPECT_EQ(out.file_path, L"b.md");

    // Go forward from B to C
    EXPECT_TRUE(hist_.GoForward({L"b.md", 20.0f}, out));
    EXPECT_EQ(out.file_path, L"c.md");
    EXPECT_FLOAT_EQ(out.scroll_y, 30.0f);

    EXPECT_FALSE(hist_.CanGoForward());
}

// ─── Same-file anchor navigation ───

TEST_F(NavHistoryTest, SameFileAnchorNavigation) {
    hist_.Push({L"readme.md", 0.0f});   // before jumping to anchor
    hist_.Push({L"readme.md", 500.0f}); // before jumping to another anchor

    NavEntry out;
    EXPECT_TRUE(hist_.GoBack({L"readme.md", 1200.0f}, out));
    EXPECT_EQ(out.file_path, L"readme.md");
    EXPECT_FLOAT_EQ(out.scroll_y, 500.0f);
}

// ─── Clear ───

TEST_F(NavHistoryTest, ClearRemovesAll) {
    hist_.Push({L"a.md", 0.0f});
    hist_.Push({L"b.md", 0.0f});

    NavEntry out;
    hist_.GoBack({L"c.md", 0.0f}, out);

    hist_.Clear();
    EXPECT_FALSE(hist_.CanGoBack());
    EXPECT_FALSE(hist_.CanGoForward());
}

// ─── Max history cap ───

TEST_F(NavHistoryTest, MaxHistoryCapsBackStack) {
    for (size_t i = 0; i < NavHistory::MAX_HISTORY + 10; ++i) {
        hist_.Push({L"file" + std::to_wstring(i) + L".md", static_cast<float>(i)});
    }
    EXPECT_EQ(hist_.BackSize(), NavHistory::MAX_HISTORY);
}

// ─── Scenario: file opened via Open File dialog / drag & drop ───
// These tests document the expected call sequences after the fix
// that ensures PushNavHistory() is called before LoadMarkdownFile()
// in Open File dialog and drag & drop code paths.

TEST_F(NavHistoryTest, OpenDialogSecondFileCanGoBack) {
    // Simulate: first file opened (no push), then second file via Ctrl+O
    // First load: current_file_ is empty → no push
    // (user is now viewing a.md)

    // Second load: current_file_ == "a.md" → push before loading b.md
    hist_.Push({L"a.md", 42.0f});

    NavEntry out;
    EXPECT_TRUE(hist_.GoBack({L"b.md", 0.0f}, out));
    EXPECT_EQ(out.file_path, L"a.md");
    EXPECT_FLOAT_EQ(out.scroll_y, 42.0f);
}

TEST_F(NavHistoryTest, DropFileSecondFileCanGoBack) {
    // Simulate: first file opened, then second file via drag & drop
    // current_file_ == "readme.md" → push before loading dropped.md
    hist_.Push({L"readme.md", 300.0f});

    NavEntry out;
    EXPECT_TRUE(hist_.GoBack({L"dropped.md", 0.0f}, out));
    EXPECT_EQ(out.file_path, L"readme.md");
    EXPECT_FLOAT_EQ(out.scroll_y, 300.0f);
}

TEST_F(NavHistoryTest, FirstLoadNoPushKeepsHistoryEmpty) {
    // Simulate: very first file load (current_file_ is empty)
    // No push should happen → history stays empty
    // (We simply don't call Push here, matching the guard !current_file_.empty())
    EXPECT_FALSE(hist_.CanGoBack());
    EXPECT_FALSE(hist_.CanGoForward());
    EXPECT_EQ(hist_.BackSize(), 0u);
}

TEST_F(NavHistoryTest, MixedEntryPointsProduceConsistentHistory) {
    // Simulate: open A (first load, no push)
    //         → open B via file pane (push A)
    //         → open C via drag & drop (push B)
    //         → open D via Ctrl+O (push C)
    hist_.Push({L"a.md", 10.0f});  // before B via file pane
    hist_.Push({L"b.md", 20.0f});  // before C via drag & drop
    hist_.Push({L"c.md", 30.0f});  // before D via Ctrl+O

    EXPECT_EQ(hist_.BackSize(), 3u);

    NavEntry out;
    // Back from D → C
    EXPECT_TRUE(hist_.GoBack({L"d.md", 40.0f}, out));
    EXPECT_EQ(out.file_path, L"c.md");
    EXPECT_FLOAT_EQ(out.scroll_y, 30.0f);

    // Back from C → B
    EXPECT_TRUE(hist_.GoBack({L"c.md", 30.0f}, out));
    EXPECT_EQ(out.file_path, L"b.md");
    EXPECT_FLOAT_EQ(out.scroll_y, 20.0f);

    // Back from B → A
    EXPECT_TRUE(hist_.GoBack({L"b.md", 20.0f}, out));
    EXPECT_EQ(out.file_path, L"a.md");
    EXPECT_FLOAT_EQ(out.scroll_y, 10.0f);

    EXPECT_FALSE(hist_.CanGoBack());

    // Forward all the way back to D
    EXPECT_TRUE(hist_.GoForward({L"a.md", 10.0f}, out));
    EXPECT_EQ(out.file_path, L"b.md");

    EXPECT_TRUE(hist_.GoForward({L"b.md", 20.0f}, out));
    EXPECT_EQ(out.file_path, L"c.md");

    EXPECT_TRUE(hist_.GoForward({L"c.md", 30.0f}, out));
    EXPECT_EQ(out.file_path, L"d.md");
    EXPECT_FLOAT_EQ(out.scroll_y, 40.0f);

    EXPECT_FALSE(hist_.CanGoForward());
}
