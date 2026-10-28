#include <gtest/gtest.h>
#include "dwrite_test_base.h"
#include "inline_math.h"
#include "syntax.h"
#include <algorithm>

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
