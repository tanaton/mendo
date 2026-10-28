#include <gtest/gtest.h>
#include "dwrite_test_base.h"
#include "inline_math.h"
#include "syntax.h"
#include <algorithm>
#include <format>
#include <iterator>
#include <optional>
#include <string>
#include <utility>
#include <vector>

class DWriteMeasurerTest : public DWriteTestBase {};

// 描画時にグリフランの文字位置とそのまま突き合わせられるよう、トークンは UTF-16 単位で持つ。
TEST_F(DWriteMeasurerTest, CodeBlockSyntaxTokensAreUtf16)
{
    auto [nodes, cache] = ParseAndLayout("```cpp\n// あい\nreturn 0;\n```\n");
    ASSERT_EQ(nodes.size(), 1u);
    const auto tokens = cache[0].syntax_tokens();
    ASSERT_FALSE(tokens.empty());

    EXPECT_EQ(tokens[0].type, SyntaxTokenType::Comment);
    EXPECT_EQ(tokens[0].start, 0u);
    EXPECT_EQ(tokens[0].length, 6u); // "// あい" (UTF-8 で 9 byte、UTF-16 で 5) と続く改行
    const auto ret = std::ranges::find(tokens, SyntaxTokenType::Keyword, &SyntaxToken::type);
    ASSERT_NE(ret, tokens.end());
    EXPECT_EQ(ret->start, 6u);
    EXPECT_EQ(ret->length, 7u); // "return" と続く空白
}

// 空白・改行だけの隙間は直前のトークンに含め、同種ならまとめる。記号を挟む隙間はそのまま残す。
TEST_F(DWriteMeasurerTest, CodeBlockSyntaxTokensAbsorbBlankGaps)
{
    auto [nodes, cache] = ParseAndLayout("```java\npublic static\n  final int x;\nint y;\n```\n");
    ASSERT_EQ(nodes.size(), 1u);
    const auto tokens = cache[0].syntax_tokens();
    ASSERT_EQ(tokens.size(), 3u);
    EXPECT_EQ(tokens[0].type, SyntaxTokenType::Keyword);
    EXPECT_EQ(tokens[0].start, 0u);
    EXPECT_EQ(tokens[0].length, 22u); // "public static\n  final "
    EXPECT_EQ(tokens[1].type, SyntaxTokenType::Type);
    EXPECT_EQ(tokens[1].start, 22u);
    EXPECT_EQ(tokens[1].length, 3u); // " x;\n" は記号を含むので伸ばさない
    EXPECT_EQ(tokens[2].type, SyntaxTokenType::Type);
    EXPECT_EQ(tokens[2].start, 29u);
}

// 左揃えで列幅に収まるセルは無限幅のまま使い、折り返す幅になったら列幅を付け、広がれば無限幅に戻す。
// 中央揃えは揃える基準が要るので常に列幅を付ける。高さは折り返しの有無に従う。
TEST_F(DWriteMeasurerTest, FittingLeftAlignedCellKeepsNaturalWidthLayout)
{
    auto [nodes, cache] = ParseAndLayout("| left | center |\n|---|:---:|\n| some words that wrap when narrow | other words that wrap when narrow |\n");
    ASSERT_EQ(nodes.size(), 1u);
    auto& entry = cache[0];
    const auto* tl = entry.table_layout();
    ASSERT_NE(tl, nullptr);
    auto* left = tl->GetCellLayout(1, 0);
    auto* center = tl->GetCellLayout(1, 1);
    ASSERT_NE(left, nullptr);
    ASSERT_NE(center, nullptr);
    const auto height = [](IDWriteTextLayout* layout) {
        DWRITE_TEXT_METRICS metrics{};
        layout->GetMetrics(&metrics);
        return metrics.height;
    };
    const float wide_height = height(left);
    EXPECT_GE(left->GetMaxWidth(), 1.0e6f);
    EXPECT_LT(center->GetMaxWidth(), 1.0e6f);

    measurer_.MeasureNode(nodes[0], entry, 300.0f, YRange::All());
    EXPECT_LT(left->GetMaxWidth(), 1.0e6f) << "収まらない幅では列幅で折り返す";
    EXPECT_GT(height(left), wide_height);

    measurer_.MeasureNode(nodes[0], entry, 800.0f, YRange::All());
    EXPECT_GE(left->GetMaxWidth(), 1.0e6f);
    EXPECT_FLOAT_EQ(height(left), wide_height);
}

class DWriteInlineMathTest : public DWriteMeasurerTest {
protected:
    void SetUp() override
    {
        DWriteMeasurerTest::SetUp();
        measurer_.SetInlineMath(&table_);
    }
    void TearDown() override
    {
        measurer_.SetInlineMath(nullptr);
        DWriteMeasurerTest::TearDown();
    }

    // [start, start+length) の文字を覆う矩形の幅の合計。
    static float RangeWidth(IDWriteTextLayout* layout, UINT32 start, UINT32 length)
    {
        DWRITE_HIT_TEST_METRICS m[8]{};
        UINT32 count = 0;
        layout->HitTestTextRange(start, length, 0.0f, 0.0f, m, 8, &count);
        float width = 0.0f;
        for (UINT32 i = 0; i < count; ++i) {
            width += m[i].width;
        }
        return width;
    }

    InlineMathTable table_;
};

// 寸法が無いうちは原文のまま組んで描画を頼み、寸法が出たら組み直して $...$ を 1 つのオブジェクトにする。
TEST_F(DWriteInlineMathTest, WaitsForMetricsThenReplacesRangeWithObject)
{
    auto [nodes, cache] = ParseAndLayout("a $x^2$ b");
    ASSERT_EQ(nodes.size(), 1u);
    EXPECT_TRUE(cache[0].pending_math);
    auto requests = table_.TakeRequests();
    ASSERT_EQ(requests.size(), 1u);
    EXPECT_EQ(requests[0].tex, "x^2");
    EXPECT_FLOAT_EQ(requests[0].font_size, theme_.font_size_body);
    EXPECT_EQ(requests[0].key, InlineMathKey("x^2", theme_.font_size_body));

    table_.Resolve(requests[0].key, { 123.0f, 30.0f, 20.0f });
    EXPECT_TRUE(cache.InvalidatePendingMathLayouts());
    EXPECT_FALSE(cache.InvalidatePendingMathLayouts()) << "印は 1 回で外す";
    ComputeLayout(engine_, nodes, cache, 800.0f);
    EXPECT_FALSE(cache[0].pending_math);
    EXPECT_TRUE(table_.TakeRequests().empty());
    EXPECT_FLOAT_EQ(RangeWidth(cache[0].text_layout(), 2, 5), 123.0f);
}

// 描けなかった数式は待たずに原文のまま組む。
TEST_F(DWriteInlineMathTest, FailedMathStaysAsText)
{
    const uint64_t key = InlineMathKey(R"(\frac{a})", theme_.font_size_body);
    table_.FindOrRequest(key, R"(\frac{a})", theme_.font_size_body);
    table_.Resolve(key, {});
    auto [nodes, cache] = ParseAndLayout(R"(a $\frac{a}$ b)");
    EXPECT_FALSE(cache[0].pending_math);
    EXPECT_GT(RangeWidth(cache[0].text_layout(), 2, 10), 30.0f);
}

// 見出しは文字が大きいので、同じ TeX でも別の大きさで頼む。表のセルの数式も待ちの印を付ける。
TEST_F(DWriteInlineMathTest, RequestsPerFontSizeAndMarksTableCells)
{
    auto [nodes, cache] = ParseAndLayout("# $x$\n\n$x$\n\n| h |\n|---|\n| $y$ |\n");
    ASSERT_EQ(nodes.size(), 3u);
    // ノードは並列に測るので、頼んだ順は決まらない。
    auto requests = table_.TakeRequests();
    std::ranges::sort(requests, std::ranges::greater{}, &InlineMathItem::font_size);
    ASSERT_EQ(requests.size(), 3u);
    EXPECT_EQ(requests[0].tex, "x");
    EXPECT_GT(requests[0].font_size, theme_.font_size_body);
    EXPECT_FLOAT_EQ(requests[1].font_size, theme_.font_size_body);
    EXPECT_FLOAT_EQ(requests[2].font_size, theme_.font_size_body);
    EXPECT_NE(requests[0].key, InlineMathKey("x", theme_.font_size_body));
    const auto* tl = cache[2].table_layout();
    ASSERT_NE(tl, nullptr);
    EXPECT_TRUE(tl->math_pending);
    EXPECT_TRUE(cache.InvalidatePendingMathLayouts());
    EXPECT_EQ(cache[2].table_layout(), nullptr);
    EXPECT_TRUE(cache[2].layout_dirty);
}

// 同じ数式は結果が出るまで 1 回だけ頼み、溜まり始めたときだけ知らせる。取り消したら頼み直せる。
TEST(InlineMathTable, RequestsOnceAndWakesWhenQueueStarts)
{
    InlineMathTable table;
    int wakes = 0;
    table.SetWake([&] { ++wakes; });
    EXPECT_FALSE(table.FindOrRequest(1, "a", 16.0f));
    EXPECT_FALSE(table.FindOrRequest(1, "a", 16.0f));
    EXPECT_FALSE(table.FindOrRequest(2, "b", 16.0f));
    EXPECT_EQ(wakes, 1);
    EXPECT_EQ(table.TakeRequests().size(), 2u);

    table.Forget(1);
    EXPECT_FALSE(table.FindOrRequest(1, "a", 16.0f));
    EXPECT_EQ(wakes, 2);
    table.Resolve(1, { 10.0f, 5.0f, 4.0f });
    const auto found = table.FindOrRequest(1, "a", 16.0f);
    ASSERT_TRUE(found);
    EXPECT_FLOAT_EQ(found->width, 10.0f);
    table.Forget(1);
    EXPECT_TRUE(table.FindOrRequest(1, "a", 16.0f)) << "描けた数式は Forget で消さない";
}

namespace {

std::vector<uint64_t> ChunkKeys(const InlineMathPlan& plan)
{
    std::vector<uint64_t> keys;
    for (const auto& c : plan.Chunks()) {
        keys.push_back(c.key);
    }
    return keys;
}

} // namespace

// 文書の順に重複を除いて並べ、文字の大きさは計測と同じ規則で決める。同じ文書なら毎回同じ組になる。
TEST(InlineMathPlan, OrdersDedupesAndUsesLayoutFontSizes)
{
    const Theme theme = GetLightTheme();
    const auto nodes = ParseNodes("# $h$\n\n$a$ $b$ $a$\n\n| $t$ |\n|---|\n| $c$ |\n");
    InlineMathPlan plan;
    plan.Build(nodes, theme);
    std::vector<std::pair<std::string, float>> items;
    for (const auto& c : plan.Chunks()) {
        for (const auto& item : c.items) {
            items.emplace_back(item.tex, item.font_size);
            EXPECT_EQ(item.key, InlineMathKey(item.tex, item.font_size));
            EXPECT_EQ(plan.ChunkOf(item.key), std::optional<size_t>{ static_cast<size_t>(&c - plan.Chunks().data()) });
        }
    }
    const std::vector<std::pair<std::string, float>> expected{
        { "h", theme.font_size_h[0] }, { "a", theme.font_size_body }, { "b", theme.font_size_body },
        { "t", theme.font_size_h[3] }, { "c", theme.font_size_body },
    };
    EXPECT_EQ(items, expected);
    EXPECT_FALSE(plan.ChunkOf(InlineMathKey("zzz", theme.font_size_body)));

    InlineMathPlan again;
    again.Build(nodes, theme);
    EXPECT_EQ(ChunkKeys(again), ChunkKeys(plan));
}

// 数式を 1 つ足しても、変わる組はその前後だけで、残りの組はディスクキャッシュに当たり続ける。組は上限を超えない。
TEST(InlineMathPlan, InsertingFormulaChangesOnlyNearbyChunks)
{
    const Theme theme = GetLightTheme();
    std::string before;
    std::string after;
    for (int i = 0; i < 3000; ++i) {
        const auto formula = std::format("${}x$ ", i);
        before += formula;
        after += formula;
        if (i == 1500) {
            after += "$inserted$ ";
        }
    }
    InlineMathPlan a;
    InlineMathPlan b;
    a.Build(ParseNodes(before), theme);
    b.Build(ParseNodes(after), theme);
    ASSERT_GT(a.Chunks().size(), 10u);
    for (const auto& c : b.Chunks()) {
        EXPECT_LE(c.items.size(), kMaxInlineMathPerChunk);
    }
    auto ka = ChunkKeys(a);
    auto kb = ChunkKeys(b);
    std::ranges::sort(ka);
    std::ranges::sort(kb);
    std::vector<uint64_t> changed;
    std::ranges::set_symmetric_difference(ka, kb, std::back_inserter(changed));
    EXPECT_LE(changed.size(), 4u);
}

// 組ごと描くとき、まだ測っていない数式も頼んだことにして、計測から二重に頼ませない。描けた数式は上書きしない。
TEST(InlineMathTable, MarkRequestedSuppressesRequestsFromMeasuring)
{
    InlineMathTable table;
    table.FindOrRequest(1, "a", 16.0f);
    table.Resolve(1, { 10.0f, 5.0f, 4.0f });
    table.TakeRequests();
    const InlineMathItem items[]{ { 1, "a", 16.0f }, { 2, "b", 16.0f } };
    table.MarkRequested(items);
    EXPECT_TRUE(table.FindOrRequest(1, "a", 16.0f));
    EXPECT_FALSE(table.FindOrRequest(2, "b", 16.0f));
    EXPECT_TRUE(table.TakeRequests().empty());
    EXPECT_TRUE(table.Resolve(2, { 10.0f, 5.0f, 4.0f }));
}

// Clear の前に頼んだ結果が後から届いても表に戻さず、呼び出し側に型を置かせない。
TEST(InlineMathTable, ResolveAfterClearIsIgnored)
{
    InlineMathTable table;
    table.FindOrRequest(1, "a", 16.0f);
    table.Clear();
    EXPECT_TRUE(table.TakeRequests().empty());
    EXPECT_FALSE(table.Resolve(1, { 10.0f, 5.0f, 4.0f }));
    EXPECT_FALSE(table.FindOrRequest(1, "a", 16.0f)) << "忘れたので頼み直す";
    EXPECT_TRUE(table.Resolve(1, { 10.0f, 5.0f, 4.0f }));
}

// デバイスロストでは寸法を残したまま、描けた数式だけを頼み直す。
TEST(InlineMathTable, RequestAllAgainKeepsMetrics)
{
    InlineMathTable table;
    table.FindOrRequest(1, "a", 16.0f);
    table.FindOrRequest(2, "b", 16.0f);
    table.FindOrRequest(3, "c", 16.0f);
    table.TakeRequests();
    table.Resolve(1, { 10.0f, 5.0f, 4.0f });
    table.Resolve(2, {});
    table.RequestAllAgain();
    const auto again = table.TakeRequests();
    ASSERT_EQ(again.size(), 1u);
    EXPECT_EQ(again[0].key, 1u);
    EXPECT_EQ(again[0].tex, "a");
    EXPECT_TRUE(table.FindOrRequest(1, "a", 16.0f));
}
