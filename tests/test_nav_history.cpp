#include <gtest/gtest.h>
#include "nav.h"
#include <deque>
#include <format>
#include <optional>
#include <random>
#include <string>

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

TEST_F(NavHistoryTest, MaxHistoryCapsForwardStack)
{
    // 戻るスタックにMAX_HISTORY+10件を積む
    for (size_t i = 0; i < NavHistory::MAX_HISTORY + 10; ++i) {
        hist_.Push({ L"file" + std::to_wstring(i) + L".md", static_cast<int>(i), 0.0f });
    }
    // 全件GoBackして進むスタックに移す
    NavEntry out;
    for (size_t i = 0; i < NavHistory::MAX_HISTORY; ++i) {
        if (!hist_.GoBack({ L"cur.md", 0, 0.0f }, out)) {
            break;
        }
    }
    EXPECT_LE(hist_.ForwardSize(), NavHistory::MAX_HISTORY);
}

TEST_F(NavHistoryTest, MaxHistoryCapsBackStack)
{
    for (size_t i = 0; i < NavHistory::MAX_HISTORY * 2; ++i) {
        hist_.Push({ L"file_" + std::to_wstring(i) + L".md", static_cast<int>(i), 0.0f });
    }
    EXPECT_EQ(hist_.BackSize(), NavHistory::MAX_HISTORY);
}

// ─── 参照モデルとの比較 (モデルベーステスト) ───
// 容量上限と戻る/進むの組み合わせは例示テストでは踏み切れない (GoForward の容量超過 0a8b2a4 は回帰テスト無しだった)。
// 素朴な deque 2 本の参照モデルとランダム操作列で突き合わせる。

namespace {

struct ModelEntry {
    std::wstring path;
    int node;
    float offset;
};

class NavHistoryModel {
public:
    explicit NavHistoryModel(size_t max) : max_(max) {}

    void Push(const ModelEntry& e)
    {
        PushCapped(back_, e);
        forward_.clear();
    }

    std::optional<ModelEntry> Transfer(bool forward, const ModelEntry& current)
    {
        auto& from = forward ? forward_ : back_;
        auto& to = forward ? back_ : forward_;
        if (from.empty()) {
            return std::nullopt;
        }
        PushCapped(to, current);
        ModelEntry out = from.back();
        from.pop_back();
        return out;
    }

    void Clear()
    {
        back_.clear();
        forward_.clear();
    }

    size_t BackSize() const noexcept
    {
        return back_.size();
    }
    size_t ForwardSize() const noexcept
    {
        return forward_.size();
    }

private:
    void PushCapped(std::deque<ModelEntry>& d, const ModelEntry& e)
    {
        d.push_back(e);
        if (d.size() > max_) {
            d.pop_front();
        }
    }

    size_t max_;
    std::deque<ModelEntry> back_;
    std::deque<ModelEntry> forward_;
};

void RunNavHistoryModel(size_t max, uint32_t seed)
{
    static const std::wstring kPaths[] = {
        L"a.md", L"C:\\docs\\long\\nested\\path\\b.md", L"c.md", L"D:\\x.md", L"e.markdown",
    };
    NavHistory hist(max);
    NavHistoryModel model(max);
    std::mt19937 rng(seed);
    auto pick = [&](int lo, int hi) {
        return std::uniform_int_distribution<int>(lo, hi)(rng);
    };

    for (int step = 0; step < 3000; ++step) {
        const ModelEntry cur{ kPaths[pick(0, 4)], pick(-1, 50), static_cast<float>(pick(-100, 100)) };
        const int op = pick(0, 99);
        std::string label;
        if (op < 35) {
            label = "Push";
            hist.Push(NavEntry(cur.path, cur.node, cur.offset));
            model.Push(cur);
        }
        else if (op < 98) {
            const bool forward = op >= 65;
            label = forward ? "GoForward" : "GoBack";
            NavEntry out;
            const NavEntry cur_entry(cur.path, cur.node, cur.offset);
            const bool moved = forward ? hist.GoForward(cur_entry, out) : hist.GoBack(cur_entry, out);
            const auto expected = model.Transfer(forward, cur);
            SCOPED_TRACE(std::format("max={} seed={} step={} op={}", max, seed, step, label));
            ASSERT_EQ(moved, expected.has_value());
            if (expected) {
                ASSERT_EQ(std::wstring(out.file_path), expected->path);
                ASSERT_EQ(out.node, expected->node);
                ASSERT_EQ(out.offset, expected->offset);
            }
        }
        else {
            label = "Clear";
            hist.Clear();
            model.Clear();
        }

        SCOPED_TRACE(std::format("max={} seed={} step={} op={}", max, seed, step, label));
        ASSERT_EQ(hist.BackSize(), model.BackSize());
        ASSERT_EQ(hist.ForwardSize(), model.ForwardSize());
        ASSERT_LE(hist.BackSize() + hist.ForwardSize(), max);
        ASSERT_EQ(hist.CanGoBack(), model.BackSize() > 0);
        ASSERT_EQ(hist.CanGoForward(), model.ForwardSize() > 0);
    }
}

} // namespace

TEST(NavHistoryModelTest, MatchesReferenceModel)
{
    for (size_t max : { size_t{ 1 }, size_t{ 3 }, size_t{ 8 } }) {
        for (uint32_t seed : { 1u, 17u, 4242u }) {
            RunNavHistoryModel(max, seed);
            if (HasFatalFailure()) {
                return;
            }
        }
    }
}
