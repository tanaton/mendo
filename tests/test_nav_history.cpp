#include <gtest/gtest.h>
#include "nav_history.h"

class NavHistoryTest : public ::testing::Test {
protected:
    NavHistory hist_;
};

// ─── 基本状態 ───

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

// ─── Push / GoBack（プッシュ / 戻る） ───

TEST_F(NavHistoryTest, PushThenGoBack) {
    hist_.Push({L"a.md", 100.0f});
    EXPECT_TRUE(hist_.CanGoBack());
    EXPECT_FALSE(hist_.CanGoForward());

    NavEntry out;
    EXPECT_TRUE(hist_.GoBack({L"b.md", 200.0f}, out));
    EXPECT_EQ(out.file_path, L"a.md");
    EXPECT_FLOAT_EQ(out.scroll_y, 100.0f);

    // 戻った後は、進むが利用可能になるべき
    EXPECT_TRUE(hist_.CanGoForward());
    EXPECT_FALSE(hist_.CanGoBack());
}

// ─── GoBack後にGoForward（戻ってから進む） ───

TEST_F(NavHistoryTest, GoBackThenGoForward) {
    hist_.Push({L"a.md", 0.0f});

    NavEntry out;
    hist_.GoBack({L"b.md", 50.0f}, out);

    EXPECT_TRUE(hist_.GoForward({L"a.md", 0.0f}, out));
    EXPECT_EQ(out.file_path, L"b.md");
    EXPECT_FLOAT_EQ(out.scroll_y, 50.0f);
}

// ─── 新規ナビゲーションで進むスタックをクリア ───

TEST_F(NavHistoryTest, PushClearsForwardStack) {
    hist_.Push({L"a.md", 0.0f});

    NavEntry out;
    hist_.GoBack({L"b.md", 0.0f}, out);
    EXPECT_TRUE(hist_.CanGoForward());

    // 新規ナビゲーションは進むスタックをクリアすべき
    hist_.Push({L"a.md", 0.0f});
    EXPECT_FALSE(hist_.CanGoForward());
}

// ─── 複数エントリ ───

TEST_F(NavHistoryTest, MultipleBackForward) {
    // シミュレーション: A を開く -> B を開く -> C を開く
    hist_.Push({L"a.md", 10.0f});  // B を開く前
    hist_.Push({L"b.md", 20.0f});  // C を開く前

    EXPECT_EQ(hist_.BackSize(), 2u);

    NavEntry out;
    // CからBへ戻る
    EXPECT_TRUE(hist_.GoBack({L"c.md", 30.0f}, out));
    EXPECT_EQ(out.file_path, L"b.md");
    EXPECT_FLOAT_EQ(out.scroll_y, 20.0f);

    // BからAへ戻る
    EXPECT_TRUE(hist_.GoBack({L"b.md", 20.0f}, out));
    EXPECT_EQ(out.file_path, L"a.md");
    EXPECT_FLOAT_EQ(out.scroll_y, 10.0f);

    EXPECT_FALSE(hist_.CanGoBack());
    EXPECT_EQ(hist_.ForwardSize(), 2u);

    // AからBへ進む
    EXPECT_TRUE(hist_.GoForward({L"a.md", 10.0f}, out));
    EXPECT_EQ(out.file_path, L"b.md");

    // BからCへ進む
    EXPECT_TRUE(hist_.GoForward({L"b.md", 20.0f}, out));
    EXPECT_EQ(out.file_path, L"c.md");
    EXPECT_FLOAT_EQ(out.scroll_y, 30.0f);

    EXPECT_FALSE(hist_.CanGoForward());
}

// ─── 同一ファイル内アンカーナビゲーション ───

TEST_F(NavHistoryTest, SameFileAnchorNavigation) {
    hist_.Push({L"readme.md", 0.0f});   // アンカーへジャンプする前
    hist_.Push({L"readme.md", 500.0f}); // 別のアンカーへジャンプする前

    NavEntry out;
    EXPECT_TRUE(hist_.GoBack({L"readme.md", 1200.0f}, out));
    EXPECT_EQ(out.file_path, L"readme.md");
    EXPECT_FLOAT_EQ(out.scroll_y, 500.0f);
}

// ─── クリア ───

TEST_F(NavHistoryTest, ClearRemovesAll) {
    hist_.Push({L"a.md", 0.0f});
    hist_.Push({L"b.md", 0.0f});

    NavEntry out;
    hist_.GoBack({L"c.md", 0.0f}, out);

    hist_.Clear();
    EXPECT_FALSE(hist_.CanGoBack());
    EXPECT_FALSE(hist_.CanGoForward());
}

// ─── 履歴の最大数制限 ───

TEST_F(NavHistoryTest, MaxHistoryCapsBackStack) {
    for (size_t i = 0; i < NavHistory::MAX_HISTORY + 10; ++i) {
        hist_.Push({L"file" + std::to_wstring(i) + L".md", static_cast<float>(i)});
    }
    EXPECT_EQ(hist_.BackSize(), NavHistory::MAX_HISTORY);
}

// ─── シナリオ: ファイルを開くダイアログ / ドラッグ＆ドロップでファイルを開く ───
// これらのテストは、ファイルを開くダイアログとドラッグ＆ドロップのコードパスで
// PushNavHistory()がLoadMarkdownFile()の前に呼ばれることを保証する
// 修正後の期待されるコールシーケンスを文書化する。

TEST_F(NavHistoryTest, OpenDialogSecondFileCanGoBack) {
    // シミュレーション: 最初のファイルを開く（pushなし）、次にCtrl+Oで2番目のファイル
    // 最初のロード: current_file_が空 → pushなし
    // （ユーザーはa.mdを閲覧中）

    // 2番目のロード: current_file_ == "a.md" → b.mdをロードする前にpush
    hist_.Push({L"a.md", 42.0f});

    NavEntry out;
    EXPECT_TRUE(hist_.GoBack({L"b.md", 0.0f}, out));
    EXPECT_EQ(out.file_path, L"a.md");
    EXPECT_FLOAT_EQ(out.scroll_y, 42.0f);
}

TEST_F(NavHistoryTest, DropFileSecondFileCanGoBack) {
    // シミュレーション: 最初のファイルを開く、次にドラッグ＆ドロップで2番目のファイル
    // current_file_ == "readme.md" → dropped.mdをロードする前にpush
    hist_.Push({L"readme.md", 300.0f});

    NavEntry out;
    EXPECT_TRUE(hist_.GoBack({L"dropped.md", 0.0f}, out));
    EXPECT_EQ(out.file_path, L"readme.md");
    EXPECT_FLOAT_EQ(out.scroll_y, 300.0f);
}

TEST_F(NavHistoryTest, FirstLoadNoPushKeepsHistoryEmpty) {
    // シミュレーション: 最初のファイルロード（current_file_が空）
    // pushは発生しないべき → 履歴は空のまま
    // （ここではPushを呼ばず、!current_file_.empty()のガードに一致）
    EXPECT_FALSE(hist_.CanGoBack());
    EXPECT_FALSE(hist_.CanGoForward());
    EXPECT_EQ(hist_.BackSize(), 0u);
}

TEST_F(NavHistoryTest, MixedEntryPointsProduceConsistentHistory) {
    // シミュレーション: Aを開く（初回ロード、pushなし）
    //         → ファイルペインからBを開く（Aをpush）
    //         → ドラッグ＆ドロップでCを開く（Bをpush）
    //         → Ctrl+OでDを開く（Cをpush）
    hist_.Push({L"a.md", 10.0f});  // ファイルペインからBを開く前
    hist_.Push({L"b.md", 20.0f});  // ドラッグ＆ドロップでCを開く前
    hist_.Push({L"c.md", 30.0f});  // Ctrl+OでDを開く前

    EXPECT_EQ(hist_.BackSize(), 3u);

    NavEntry out;
    // DからCへ戻る
    EXPECT_TRUE(hist_.GoBack({L"d.md", 40.0f}, out));
    EXPECT_EQ(out.file_path, L"c.md");
    EXPECT_FLOAT_EQ(out.scroll_y, 30.0f);

    // CからBへ戻る
    EXPECT_TRUE(hist_.GoBack({L"c.md", 30.0f}, out));
    EXPECT_EQ(out.file_path, L"b.md");
    EXPECT_FLOAT_EQ(out.scroll_y, 20.0f);

    // BからAへ戻る
    EXPECT_TRUE(hist_.GoBack({L"b.md", 20.0f}, out));
    EXPECT_EQ(out.file_path, L"a.md");
    EXPECT_FLOAT_EQ(out.scroll_y, 10.0f);

    EXPECT_FALSE(hist_.CanGoBack());

    // Dまで全て進む
    EXPECT_TRUE(hist_.GoForward({L"a.md", 10.0f}, out));
    EXPECT_EQ(out.file_path, L"b.md");

    EXPECT_TRUE(hist_.GoForward({L"b.md", 20.0f}, out));
    EXPECT_EQ(out.file_path, L"c.md");

    EXPECT_TRUE(hist_.GoForward({L"c.md", 30.0f}, out));
    EXPECT_EQ(out.file_path, L"d.md");
    EXPECT_FLOAT_EQ(out.scroll_y, 40.0f);

    EXPECT_FALSE(hist_.CanGoForward());
}
