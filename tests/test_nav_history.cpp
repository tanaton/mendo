#include <gtest/gtest.h>
#include "nav_history.h"

class NavHistoryTest : public ::testing::Test {
protected:
    NavHistory hist_;
};

// ─── 基本状態 ───

TEST_F(NavHistoryTest, InitiallyEmpty)
{
    EXPECT_FALSE(hist_.CanGoBack());
    EXPECT_FALSE(hist_.CanGoForward());
    EXPECT_EQ(hist_.BackSize(), 0u);
    EXPECT_EQ(hist_.ForwardSize(), 0u);
}

TEST_F(NavHistoryTest, GoBackOnEmptyReturnsFalse)
{
    NavEntry out;
    EXPECT_FALSE(hist_.GoBack({ L"a.md", 0, 0.0f }, out));
}

TEST_F(NavHistoryTest, GoForwardOnEmptyReturnsFalse)
{
    NavEntry out;
    EXPECT_FALSE(hist_.GoForward({ L"a.md", 0, 0.0f }, out));
}

// ─── Push / GoBack（プッシュ / 戻る） ───

TEST_F(NavHistoryTest, PushThenGoBack)
{
    hist_.Push({ L"a.md", 3, 25.0f });
    EXPECT_TRUE(hist_.CanGoBack());
    EXPECT_FALSE(hist_.CanGoForward());

    NavEntry out;
    EXPECT_TRUE(hist_.GoBack({ L"b.md", 7, 12.0f }, out));
    EXPECT_EQ(out.file_path, L"a.md");
    EXPECT_EQ(out.node, 3);
    EXPECT_FLOAT_EQ(out.offset, 25.0f);

    // 戻った後は、進むが利用可能になるべき
    EXPECT_TRUE(hist_.CanGoForward());
    EXPECT_FALSE(hist_.CanGoBack());
}

// ─── GoBack後にGoForward（戻ってから進む） ───

TEST_F(NavHistoryTest, GoBackThenGoForward)
{
    hist_.Push({ L"a.md", 0, 0.0f });

    NavEntry out;
    hist_.GoBack({ L"b.md", 2, 10.0f }, out);

    EXPECT_TRUE(hist_.GoForward({ L"a.md", 0, 0.0f }, out));
    EXPECT_EQ(out.file_path, L"b.md");
    EXPECT_EQ(out.node, 2);
    EXPECT_FLOAT_EQ(out.offset, 10.0f);
}

// ─── 新規ナビゲーションで進むスタックをクリア ───

TEST_F(NavHistoryTest, PushClearsForwardStack)
{
    hist_.Push({ L"a.md", 0, 0.0f });

    NavEntry out;
    hist_.GoBack({ L"b.md", 0, 0.0f }, out);
    EXPECT_TRUE(hist_.CanGoForward());

    // 新規ナビゲーションは進むスタックをクリアすべき
    hist_.Push({ L"a.md", 0, 0.0f });
    EXPECT_FALSE(hist_.CanGoForward());
}

// ─── 複数エントリ ───

TEST_F(NavHistoryTest, MultipleBackForward)
{
    // シミュレーション: A を開く -> B を開く -> C を開く
    hist_.Push({ L"a.md", 1, 2.0f });   // B を開く前
    hist_.Push({ L"b.md", 3, 4.0f });   // C を開く前

    EXPECT_EQ(hist_.BackSize(), 2u);

    NavEntry out;
    // CからBへ戻る
    EXPECT_TRUE(hist_.GoBack({ L"c.md", 5, 6.0f }, out));
    EXPECT_EQ(out.file_path, L"b.md");
    EXPECT_EQ(out.node, 3);
    EXPECT_FLOAT_EQ(out.offset, 4.0f);

    // BからAへ戻る
    EXPECT_TRUE(hist_.GoBack({ L"b.md", 3, 4.0f }, out));
    EXPECT_EQ(out.file_path, L"a.md");
    EXPECT_EQ(out.node, 1);
    EXPECT_FLOAT_EQ(out.offset, 2.0f);

    EXPECT_FALSE(hist_.CanGoBack());
    EXPECT_EQ(hist_.ForwardSize(), 2u);

    // AからBへ進む
    EXPECT_TRUE(hist_.GoForward({ L"a.md", 1, 2.0f }, out));
    EXPECT_EQ(out.file_path, L"b.md");

    // BからCへ進む
    EXPECT_TRUE(hist_.GoForward({ L"b.md", 3, 4.0f }, out));
    EXPECT_EQ(out.file_path, L"c.md");
    EXPECT_EQ(out.node, 5);
    EXPECT_FLOAT_EQ(out.offset, 6.0f);

    EXPECT_FALSE(hist_.CanGoForward());
}

// ─── 同一ファイル内アンカーナビゲーション ───

TEST_F(NavHistoryTest, SameFileAnchorNavigation)
{
    hist_.Push({ L"readme.md", 0, 0.0f });    // アンカーへジャンプする前
    hist_.Push({ L"readme.md", 12, 30.0f });  // 別のアンカーへジャンプする前

    NavEntry out;
    EXPECT_TRUE(hist_.GoBack({ L"readme.md", 25, 0.0f }, out));
    EXPECT_EQ(out.file_path, L"readme.md");
    EXPECT_EQ(out.node, 12);
    EXPECT_FLOAT_EQ(out.offset, 30.0f);
}

// ─── クリア ───

TEST_F(NavHistoryTest, ClearRemovesAll)
{
    hist_.Push({ L"a.md", 0, 0.0f });
    hist_.Push({ L"b.md", 0, 0.0f });

    NavEntry out;
    hist_.GoBack({ L"c.md", 0, 0.0f }, out);

    hist_.Clear();
    EXPECT_FALSE(hist_.CanGoBack());
    EXPECT_FALSE(hist_.CanGoForward());
}

// ─── 履歴の最大数制限 ───

TEST_F(NavHistoryTest, MaxHistoryCapsBackStack)
{
    for (size_t i = 0; i < NavHistory::MAX_HISTORY + 10; ++i) {
        hist_.Push({ L"file" + std::to_wstring(i) + L".md", static_cast<int>(i), 0.0f });
    }
    EXPECT_EQ(hist_.BackSize(), NavHistory::MAX_HISTORY);
}

TEST_F(NavHistoryTest, MaxHistoryCapsForwardStack)
{
    // 戻るスタックにMAX_HISTORY+10件を積む
    for (size_t i = 0; i < NavHistory::MAX_HISTORY + 10; ++i) {
        hist_.Push({ L"file" + std::to_wstring(i) + L".md", static_cast<int>(i), 0.0f });
    }
    // 全件GoBackして進むスタックに移す
    NavEntry out;
    for (size_t i = 0; i < NavHistory::MAX_HISTORY; ++i) {
        if (!hist_.GoBack({ L"cur.md", 0, 0.0f }, out)) break;
    }
    EXPECT_LE(hist_.ForwardSize(), NavHistory::MAX_HISTORY);
}

// ─── シナリオ: ファイルを開くダイアログ / ドラッグ＆ドロップでファイルを開く ───
// これらのテストは、ファイルを開くダイアログとドラッグ＆ドロップのコードパスで
// PushNavHistory()がLoadMarkdownFile()の前に呼ばれることを保証する
// 修正後の期待されるコールシーケンスを文書化する。

TEST_F(NavHistoryTest, OpenDialogSecondFileCanGoBack)
{
    // シミュレーション: 最初のファイルを開く（pushなし）、次にCtrl+Oで2番目のファイル
    // 最初のロード: current_file_が空 → pushなし
    // （ユーザーはa.mdを閲覧中）

    // 2番目のロード: current_file_ == "a.md" → b.mdをロードする前にpush
    hist_.Push({ L"a.md", 2, 8.0f });

    NavEntry out;
    EXPECT_TRUE(hist_.GoBack({ L"b.md", 0, 0.0f }, out));
    EXPECT_EQ(out.file_path, L"a.md");
    EXPECT_EQ(out.node, 2);
    EXPECT_FLOAT_EQ(out.offset, 8.0f);
}

TEST_F(NavHistoryTest, DropFileSecondFileCanGoBack)
{
    // シミュレーション: 最初のファイルを開く、次にドラッグ＆ドロップで2番目のファイル
    // current_file_ == "readme.md" → dropped.mdをロードする前にpush
    hist_.Push({ L"readme.md", 8, 40.0f });

    NavEntry out;
    EXPECT_TRUE(hist_.GoBack({ L"dropped.md", 0, 0.0f }, out));
    EXPECT_EQ(out.file_path, L"readme.md");
    EXPECT_EQ(out.node, 8);
    EXPECT_FLOAT_EQ(out.offset, 40.0f);
}

TEST_F(NavHistoryTest, FirstLoadNoPushKeepsHistoryEmpty)
{
    // シミュレーション: 最初のファイルロード（current_file_が空）
    // pushは発生しないべき → 履歴は空のまま
    // （ここではPushを呼ばず、!current_file_.empty()のガードに一致）
    EXPECT_FALSE(hist_.CanGoBack());
    EXPECT_FALSE(hist_.CanGoForward());
    EXPECT_EQ(hist_.BackSize(), 0u);
}

// ─── インターン化されたパスの回収（Medium-7 回帰） ───
// 履歴件数は MAX_HISTORY で抑えられているが、以前は intern 済みパスが
// Clear() まで永久に残っていた。長時間セッションで多数のファイルを跨ぐと、
// 履歴長が一定でも文字列メモリが増え続ける問題があった。

TEST_F(NavHistoryTest, EvictedPathsAreReclaimed)
{
    // MAX_HISTORY * 2 件の異なるパスを push する。
    // 最初の MAX_HISTORY 件は back_stack の cap で押し出され、
    // 参照ゼロになって intern table から消えるはず。
    for (size_t i = 0; i < NavHistory::MAX_HISTORY * 2; ++i) {
        hist_.Push({ L"file_" + std::to_wstring(i) + L".md", static_cast<int>(i), 0.0f });
    }
    EXPECT_EQ(hist_.BackSize(), NavHistory::MAX_HISTORY);
    // intern table のサイズは最大でも back_stack + forward_stack の合計に収まる
    EXPECT_LE(hist_.InternedPathCount(), NavHistory::MAX_HISTORY + hist_.ForwardSize());
}

TEST_F(NavHistoryTest, SamePathDoesNotInflateInternTable)
{
    // 同じパスを繰り返し push しても intern table は 1 件のまま
    for (size_t i = 0; i < 100; ++i) {
        hist_.Push({ L"same.md", static_cast<int>(i), 0.0f });
    }
    EXPECT_EQ(hist_.InternedPathCount(), 1u);
}

TEST_F(NavHistoryTest, ClearedForwardStackReleasesPaths)
{
    // 戻る → 進むスタックに distinct なパスを溜める → 新規 push でクリア
    for (size_t i = 0; i < 5; ++i) {
        hist_.Push({ L"f" + std::to_wstring(i) + L".md", 0, 0.0f });
    }
    NavEntry out;
    // GoBack のたびに current として渡す path も毎回ユニークにする
    for (size_t i = 0; i < 5; ++i) {
        hist_.GoBack({ L"x" + std::to_wstring(i) + L".md", 0, 0.0f }, out);
    }
    EXPECT_EQ(hist_.ForwardSize(), 5u);
    // この時点では back_stack は空、forward_stack に x0..x4 のみが intern される
    EXPECT_EQ(hist_.InternedPathCount(), 5u);

    // 新規 push → forward_stack の x0..x4 が解放され、back に new.md が入る
    hist_.Push({ L"new.md", 0, 0.0f });
    EXPECT_EQ(hist_.InternedPathCount(), 1u);
}

TEST_F(NavHistoryTest, MixedEntryPointsProduceConsistentHistory)
{
    // シミュレーション: Aを開く（初回ロード、pushなし）
    //         → ファイルペインからBを開く（Aをpush）
    //         → ドラッグ＆ドロップでCを開く（Bをpush）
    //         → Ctrl+OでDを開く（Cをpush）
    hist_.Push({ L"a.md", 1, 0.0f });   // ファイルペインからBを開く前
    hist_.Push({ L"b.md", 2, 0.0f });   // ドラッグ＆ドロップでCを開く前
    hist_.Push({ L"c.md", 3, 0.0f });   // Ctrl+OでDを開く前

    EXPECT_EQ(hist_.BackSize(), 3u);

    NavEntry out;
    // DからCへ戻る
    EXPECT_TRUE(hist_.GoBack({ L"d.md", 4, 0.0f }, out));
    EXPECT_EQ(out.file_path, L"c.md");
    EXPECT_EQ(out.node, 3);

    // CからBへ戻る
    EXPECT_TRUE(hist_.GoBack({ L"c.md", 3, 0.0f }, out));
    EXPECT_EQ(out.file_path, L"b.md");
    EXPECT_EQ(out.node, 2);

    // BからAへ戻る
    EXPECT_TRUE(hist_.GoBack({ L"b.md", 2, 0.0f }, out));
    EXPECT_EQ(out.file_path, L"a.md");
    EXPECT_EQ(out.node, 1);

    EXPECT_FALSE(hist_.CanGoBack());

    // Dまで全て進む
    EXPECT_TRUE(hist_.GoForward({ L"a.md", 1, 0.0f }, out));
    EXPECT_EQ(out.file_path, L"b.md");

    EXPECT_TRUE(hist_.GoForward({ L"b.md", 2, 0.0f }, out));
    EXPECT_EQ(out.file_path, L"c.md");

    EXPECT_TRUE(hist_.GoForward({ L"c.md", 3, 0.0f }, out));
    EXPECT_EQ(out.file_path, L"d.md");
    EXPECT_EQ(out.node, 4);

    EXPECT_FALSE(hist_.CanGoForward());
}
