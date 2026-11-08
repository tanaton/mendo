#include <gtest/gtest.h>
#include "nav.h"
#include <algorithm>
#include <format>
#include <optional>
#include <random>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

class NavHistoryTest : public ::testing::Test {
protected:
    NavHistory hist_;
};

// ─── 基本状態 ───

// 空の履歴では戻るも進むもできない。
TEST_F(NavHistoryTest, InitiallyEmpty)
{
    EXPECT_FALSE(hist_.CanGoBack());
    EXPECT_FALSE(hist_.CanGoForward());
    EXPECT_EQ(hist_.BackSize(), 0u);
    EXPECT_EQ(hist_.ForwardSize(), 0u);
    NavEntry out;
    EXPECT_FALSE(hist_.GoBack({ L"a.md", 0, 0.0f }, out));
    EXPECT_FALSE(hist_.GoForward({ L"a.md", 0, 0.0f }, out));
}

// ─── 新規ナビゲーションで進むスタックをクリア ───

TEST_F(NavHistoryTest, PushClearsForwardStack)
{
    hist_.Push({ L"a.md", 0, 0.0f });

    NavEntry out;
    hist_.GoBack({ L"b.md", 0, 0.0f }, out);
    hist_.CommitOpened({ L"b.md", 0, 0.0f });
    EXPECT_TRUE(hist_.CanGoForward());

    // 新規ナビゲーションは進むスタックをクリアすべき
    hist_.Push({ L"a.md", 0, 0.0f });
    EXPECT_FALSE(hist_.CanGoForward());
}

// ─── 戻る / 進む ───

TEST_F(NavHistoryTest, MultipleBackForward)
{
    // シミュレーション: A を開く -> B を開く -> C を開く
    hist_.Push({ L"a.md", 1, 2.0f });   // B を開く前
    hist_.Push({ L"b.md", 3, 4.0f });   // C を開く前

    EXPECT_EQ(hist_.BackSize(), 2u);
    EXPECT_FALSE(hist_.CanGoForward());

    NavEntry out;
    // CからBへ戻る
    EXPECT_TRUE(hist_.GoBack({ L"c.md", 5, 6.0f }, out));
    EXPECT_EQ(out.file_path, L"b.md");
    EXPECT_EQ(out.node, 3);
    EXPECT_FLOAT_EQ(out.offset, 4.0f);
    EXPECT_TRUE(hist_.CanGoForward());
    hist_.CommitOpened({ L"c.md", 5, 6.0f });

    // BからAへ戻る
    EXPECT_TRUE(hist_.GoBack({ L"b.md", 3, 4.0f }, out));
    EXPECT_EQ(out.file_path, L"a.md");
    EXPECT_EQ(out.node, 1);
    EXPECT_FLOAT_EQ(out.offset, 2.0f);
    hist_.CommitOpened({ L"b.md", 3, 4.0f });

    EXPECT_FALSE(hist_.CanGoBack());
    EXPECT_EQ(hist_.ForwardSize(), 2u);

    // AからBへ進む
    EXPECT_TRUE(hist_.GoForward({ L"a.md", 1, 2.0f }, out));
    EXPECT_EQ(out.file_path, L"b.md");
    hist_.CommitOpened({ L"a.md", 1, 2.0f });

    // BからCへ進む
    EXPECT_TRUE(hist_.GoForward({ L"b.md", 3, 4.0f }, out));
    EXPECT_EQ(out.file_path, L"c.md");
    EXPECT_EQ(out.node, 5);
    EXPECT_FLOAT_EQ(out.offset, 6.0f);

    EXPECT_FALSE(hist_.CanGoForward());
}

// ─── ロード中に次の操作 ───

// B のロード中に C を開いても、表示中の A は 1 回だけ積まれ、C が開けなければ B を開く前に戻る。
TEST_F(NavHistoryTest, PushPendingWhilePendingReplacesPreviousOp)
{
    hist_.Push({ L"z.md", 0, 0.0f });
    hist_.PushPending({ L"a.md", 1, 0.0f });
    hist_.PushPending({ L"a.md", 1, 0.0f });
    EXPECT_EQ(hist_.BackSize(), 2u);

    hist_.RollbackPending();
    EXPECT_EQ(hist_.BackSize(), 1u);
    EXPECT_FALSE(hist_.CanGoForward());
}

// 戻るを素早く 2 回押すと、ロード中の移動先から続けて戻る。
TEST_F(NavHistoryTest, ChainedGoBackWhilePending)
{
    hist_.Push({ L"x.md", 1, 0.0f });
    hist_.Push({ L"y.md", 2, 0.0f });

    NavEntry out;
    ASSERT_TRUE(hist_.GoBack({ L"a.md", 3, 0.0f }, out));
    EXPECT_EQ(out.file_path, L"y.md");
    // 文書はまだ A のまま。
    ASSERT_TRUE(hist_.GoBack({ L"a.md", 3, 0.0f }, out));
    EXPECT_EQ(out.file_path, L"x.md");
    hist_.CommitPending();

    EXPECT_FALSE(hist_.CanGoBack());
    ASSERT_EQ(hist_.ForwardSize(), 2u);
    ASSERT_TRUE(hist_.GoForward({ L"x.md", 1, 0.0f }, out));
    EXPECT_EQ(out.file_path, L"y.md");
    ASSERT_TRUE(hist_.GoForward({ L"y.md", 2, 0.0f }, out));
    EXPECT_EQ(out.file_path, L"a.md");
}

TEST_F(NavHistoryTest, ChainedGoBackRollsBackTogether)
{
    hist_.Push({ L"x.md", 1, 0.0f });
    hist_.Push({ L"y.md", 2, 0.0f });

    NavEntry out;
    ASSERT_TRUE(hist_.GoBack({ L"a.md", 3, 0.0f }, out));
    ASSERT_TRUE(hist_.GoBack({ L"a.md", 3, 0.0f }, out));
    hist_.RollbackPending();

    EXPECT_EQ(hist_.BackSize(), 2u);
    EXPECT_FALSE(hist_.CanGoForward());
}

// ─── ロード中に表示中の文書の中で移動 ───

namespace {

// 戻るを続けて押し、たどった位置を新しい順に返す。
std::vector<NavEntry> DrainBack(NavHistory& hist, NavEntry current)
{
    std::vector<NavEntry> out;
    NavEntry e;
    while (hist.GoBack(current, e)) {
        hist.CommitOpened(current);
        out.push_back(e);
        current = e;
    }
    return out;
}

} // namespace

// B を開いている間に A の中で 1 → 5 へ飛ぶと、A@1 は飛ぶ前の位置、A@5 は B に差し替えた時点の位置として 1 回ずつ入る。
TEST_F(NavHistoryTest, PushWhileOpeningKeepsJumpBeforeOpen)
{
    hist_.Push({ L"z.md", 0, 0.0f });
    hist_.PushPending({ L"a.md", 1, 0.0f });
    hist_.Push({ L"a.md", 1, 0.0f });
    hist_.CommitOpened({ L"a.md", 5, 0.0f });

    const auto back = DrainBack(hist_, { L"b.md", 0, 0.0f });
    ASSERT_EQ(back.size(), 3u);
    EXPECT_EQ(back[0].node, 5);
    EXPECT_EQ(back[1].node, 1);
    EXPECT_EQ(back[2].file_path, L"z.md");
}

// 開けなかったら開く分だけを取り消し、表示中の文書の中の移動は残す。
TEST_F(NavHistoryTest, RollbackAfterPushWhileOpeningKeepsJump)
{
    hist_.PushPending({ L"a.md", 1, 0.0f });
    hist_.Push({ L"a.md", 1, 0.0f });
    hist_.RollbackPending();

    EXPECT_EQ(hist_.BackSize(), 1u);
    EXPECT_FALSE(hist_.CanGoForward());
    NavEntry out;
    ASSERT_TRUE(hist_.GoBack({ L"a.md", 5, 0.0f }, out));
    EXPECT_EQ(out.node, 1);
}

// 戻るの読み込み中にスクロールしても、表示していた文書は差し替えた時点の位置で進む履歴に入る。
TEST_F(NavHistoryTest, CommitOpenedRecordsPositionAtReplacement)
{
    hist_.Push({ L"x.md", 0, 0.0f });
    NavEntry out;
    ASSERT_TRUE(hist_.GoBack({ L"a.md", 1, 0.0f }, out));
    hist_.CommitOpened({ L"a.md", 7, 0.0f });

    ASSERT_TRUE(hist_.GoForward({ L"x.md", 0, 0.0f }, out));
    EXPECT_EQ(out.file_path, L"a.md");
    EXPECT_EQ(out.node, 7);
}

// 戻るの読み込み中に A の中で 1 → 5 へ飛ぶと、飛んだ移動は戻るより前の移動になる。移動先 Y の先には A@1、A@5 の順に並ぶ。
TEST_F(NavHistoryTest, PushWhileGoingBackInsertsJumpAfterTarget)
{
    hist_.Push({ L"x.md", 0, 0.0f });
    hist_.Push({ L"y.md", 0, 0.0f });
    NavEntry out;
    ASSERT_TRUE(hist_.GoBack({ L"a.md", 1, 0.0f }, out));
    ASSERT_EQ(out.file_path, L"y.md");
    hist_.Push({ L"a.md", 1, 0.0f });
    hist_.CommitOpened({ L"a.md", 5, 0.0f });

    ASSERT_EQ(hist_.BackSize(), 1u);
    ASSERT_EQ(hist_.ForwardSize(), 2u);
    ASSERT_TRUE(hist_.GoForward({ L"y.md", 0, 0.0f }, out));
    EXPECT_EQ(out.node, 1);
    hist_.CommitOpened({ L"y.md", 0, 0.0f });
    ASSERT_TRUE(hist_.GoForward(NavEntry{ out }, out));
    EXPECT_EQ(out.node, 5);
}

// 進むの読み込み中に表示中の文書の中で飛ぶと、進む履歴は消え、移動先は開くのと同じになる。
TEST_F(NavHistoryTest, PushWhileGoingForwardBecomesOpen)
{
    hist_.Push({ L"a.md", 0, 0.0f });
    NavEntry out;
    ASSERT_TRUE(hist_.GoBack({ L"g.md", 0, 0.0f }, out));
    hist_.CommitOpened({ L"g.md", 0, 0.0f });
    ASSERT_TRUE(hist_.GoForward({ L"a.md", 1, 0.0f }, out));
    ASSERT_EQ(out.file_path, L"g.md");
    hist_.Push({ L"a.md", 1, 0.0f });
    hist_.CommitOpened({ L"a.md", 5, 0.0f });

    EXPECT_FALSE(hist_.CanGoForward());
    const auto back = DrainBack(hist_, { L"g.md", 0, 0.0f });
    ASSERT_EQ(back.size(), 2u);
    EXPECT_EQ(back[0].node, 5);
    EXPECT_EQ(back[1].node, 1);
}

// 戻るの読み込み中に進むで表示中の文書へ戻ったら、読み込まずに確定し、今の位置に留まる。
TEST_F(NavHistoryTest, ReturningToShownDocumentSettlesAtCurrentPosition)
{
    hist_.Push({ L"x.md", 0, 0.0f });
    NavEntry out;
    ASSERT_TRUE(hist_.GoBack({ L"a.md", 1, 0.0f }, out));
    ASSERT_TRUE(hist_.GoForward({ L"a.md", 9, 0.0f }, out));
    EXPECT_EQ(out.file_path, L"a.md");
    EXPECT_EQ(out.node, 9);
    EXPECT_EQ(hist_.BackSize(), 1u);
    EXPECT_FALSE(hist_.CanGoForward());
}

// ─── 履歴の最大数制限 ───

TEST_F(NavHistoryTest, MaxHistoryCapsBackStack)
{
    for (size_t i = 0; i < NavHistory::MAX_HISTORY * 2; ++i) {
        hist_.Push({ L"file_" + std::to_wstring(i) + L".md", static_cast<int>(i), 0.0f });
    }
    ASSERT_EQ(hist_.BackSize(), NavHistory::MAX_HISTORY);

    // 上限を超えたら最も古い履歴から捨てる。
    const int newest = static_cast<int>(NavHistory::MAX_HISTORY * 2) - 1;
    const int oldest_kept = static_cast<int>(NavHistory::MAX_HISTORY);
    NavEntry out;
    ASSERT_TRUE(hist_.GoBack({ L"now.md", 0, 0.0f }, out));
    EXPECT_EQ(out.node, newest);
    while (hist_.CanGoBack()) {
        ASSERT_TRUE(hist_.GoBack(NavEntry{ out }, out));
    }
    EXPECT_EQ(out.node, oldest_kept);
}

// 上限で押し出された最古の履歴も、Rollback で戻る。
TEST_F(NavHistoryTest, RollbackAtCapRestoresEvictedOldest)
{
    for (size_t i = 0; i < NavHistory::MAX_HISTORY; ++i) {
        hist_.Push({ L"file.md", static_cast<int>(i), 0.0f });
    }
    hist_.PushPending({ L"file.md", -1, 0.0f });
    ASSERT_EQ(hist_.BackSize(), NavHistory::MAX_HISTORY);
    hist_.RollbackPending();
    ASSERT_EQ(hist_.BackSize(), NavHistory::MAX_HISTORY);

    NavEntry out;
    ASSERT_TRUE(hist_.GoBack({ L"now.md", 0, 0.0f }, out));
    EXPECT_EQ(out.node, static_cast<int>(NavHistory::MAX_HISTORY) - 1);
    while (hist_.CanGoBack()) {
        ASSERT_TRUE(hist_.GoBack(NavEntry{ out }, out));
    }
    EXPECT_EQ(out.node, 0);
}

// ─── ランダム操作列の性質 ───
// 容量上限と戻る/進むの組み合わせは例示テストでは踏み切れない (GoForward の容量超過 0a8b2a4 は回帰テスト無しだった)。

namespace {

void RunNavHistoryProperties(uint32_t seed)
{
    constexpr size_t max = NavHistory::MAX_HISTORY;
    NavHistory hist;
    std::mt19937 rng(seed);
    auto pick = [&](int lo, int hi) {
        return std::uniform_int_distribution<int>(lo, hi)(rng);
    };
    // 最初の仮の操作の前のサイズ。取り消しでここへ戻る。
    std::optional<std::pair<size_t, size_t>> before_pending;

    // 一様に選ぶと上限に届く前に Clear される。積むだけ・戻るだけ・進むだけの区間を混ぜ、上限と空の両端を踏む。
    constexpr std::pair<int, int> kOpRanges[] = { { 0, 48 }, { 49, 72 }, { 73, 97 }, { 0, 99 } };
    std::pair<int, int> op_range = kOpRanges[3];
    int segment_left = 0;
    bool reached_max = false;
    int next_node = 0;

    // 上限に届くのは seed あたり 2〜4 回 (seed=1 の最初は 17020 歩目) なので、歩数は減らさない。
    for (int step = 0; step < 30000; ++step) {
        if (segment_left-- == 0) {
            op_range = kOpRanges[pick(0, 3)];
            segment_left = pick(1, 3 * static_cast<int>(max));
        }
        const NavEntry cur{ L"a.md", next_node++, 0.0f };
        const int op = pick(op_range.first, op_range.second);
        std::string_view label;
        // 9 万ステップで毎回文字列を組むと遅いので、失敗したときだけ組む。
        const auto ctx = [&] { return std::format("seed={} step={} op={}", seed, step, label); };
        if (op < 25) {
            label = "Push";
            hist.Push(cur);
            // 確定待ちの間は、取り消しても残る移動として積む。
            if (before_pending) {
                *before_pending = { std::min(before_pending->first + 1, max), 0 };
            }
            else {
                ASSERT_FALSE(hist.CanGoForward()) << ctx();
            }
        }
        else if (op < 35) {
            label = "PushPending";
            // 前の仮の操作を取り消してから積むので、戻り先は変わらない。
            if (!before_pending) {
                before_pending.emplace(hist.BackSize(), hist.ForwardSize());
            }
            hist.PushPending(cur);
            ASSERT_FALSE(hist.CanGoForward()) << ctx();
        }
        else if (op < 38) {
            label = "CommitPending";
            hist.CommitPending();
            before_pending.reset();
        }
        else if (op < 41) {
            label = "CommitOpened";
            const auto sizes = std::pair(hist.BackSize(), hist.ForwardSize());
            hist.CommitOpened(cur);
            before_pending.reset();
            ASSERT_EQ(std::pair(hist.BackSize(), hist.ForwardSize()), sizes) << ctx();
        }
        else if (op < 49) {
            label = "RollbackPending";
            hist.RollbackPending();
            if (before_pending) {
                ASSERT_EQ(std::pair(hist.BackSize(), hist.ForwardSize()), *before_pending) << ctx();
                before_pending.reset();
            }
        }
        else if (op < 98) {
            const bool forward = op >= 73;
            label = forward ? "GoForward" : "GoBack";
            const size_t back = hist.BackSize();
            const size_t fwd = hist.ForwardSize();
            const bool can_move = forward ? hist.CanGoForward() : hist.CanGoBack();
            NavEntry out;
            const bool moved = forward ? hist.GoForward(cur, out) : hist.GoBack(cur, out);
            // 確定待ちの操作があると先に取り消すことがあるので、確定済みの状態からの移動だけ差を確かめる。
            if (!before_pending) {
                ASSERT_EQ(moved, can_move) << ctx();
                if (moved) {
                    ASSERT_EQ(hist.BackSize(), forward ? back + 1 : back - 1) << ctx();
                    ASSERT_EQ(hist.ForwardSize(), forward ? fwd - 1 : fwd + 1) << ctx();
                    before_pending.emplace(back, fwd);
                }
            }
        }
        else {
            label = "Clear";
            hist = NavHistory{};
            before_pending.reset();
        }

        ASSERT_LE(hist.BackSize() + hist.ForwardSize(), max) << ctx();
        reached_max |= hist.BackSize() + hist.ForwardSize() == max;
    }
    EXPECT_TRUE(reached_max) << "seed=" << seed << " で上限に届いていない";
}

} // namespace

TEST(NavHistoryPropertyTest, HoldsOverRandomOperations)
{
    for (uint32_t seed : { 1u, 17u, 4242u }) {
        RunNavHistoryProperties(seed);
        if (HasFatalFailure()) {
            return;
        }
    }
}
