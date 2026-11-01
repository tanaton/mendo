#include <gtest/gtest.h>
#include "nav.h"
#include <format>
#include <optional>
#include <random>
#include <string>
#include <utility>

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

    for (int step = 0; step < 30000; ++step) {
        if (segment_left-- == 0) {
            op_range = kOpRanges[pick(0, 3)];
            segment_left = pick(1, 3 * static_cast<int>(max));
        }
        const NavEntry cur{ L"a.md", next_node++, 0.0f };
        const int op = pick(op_range.first, op_range.second);
        std::string label;
        if (op < 25) {
            label = "Push";
            hist.Push(cur);
            before_pending.reset();
            ASSERT_FALSE(hist.CanGoForward());
        }
        else if (op < 35) {
            label = "PushPending";
            // 前の仮の操作を取り消してから積むので、戻り先は変わらない。
            if (!before_pending) {
                before_pending.emplace(hist.BackSize(), hist.ForwardSize());
            }
            hist.PushPending(cur);
            ASSERT_FALSE(hist.CanGoForward());
        }
        else if (op < 41) {
            label = "CommitPending";
            hist.CommitPending();
            before_pending.reset();
        }
        else if (op < 49) {
            label = "RollbackPending";
            hist.RollbackPending();
            if (before_pending) {
                ASSERT_EQ(std::pair(hist.BackSize(), hist.ForwardSize()), *before_pending);
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
                ASSERT_EQ(moved, can_move);
                if (moved) {
                    ASSERT_EQ(hist.BackSize(), forward ? back + 1 : back - 1);
                    ASSERT_EQ(hist.ForwardSize(), forward ? fwd - 1 : fwd + 1);
                    before_pending.emplace(back, fwd);
                }
            }
        }
        else {
            label = "Clear";
            hist = NavHistory{};
            before_pending.reset();
        }

        SCOPED_TRACE(std::format("seed={} step={} op={}", seed, step, label));
        ASSERT_LE(hist.BackSize() + hist.ForwardSize(), max);
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
