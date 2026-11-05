#include <gtest/gtest.h>
#include "dwrite_test_base.h"
#include "doc_dwrite_bridge.h"
#include "inline_math.h"
#include "syntax.h"
#include <algorithm>
#include <format>
#include <iterator>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
#include "mock_text_measurer.h"

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

    measurer_.MeasureNode(nodes[0], entry, 300.0f);
    EXPECT_LT(left->GetMaxWidth(), 1.0e6f) << "収まらない幅では列幅で折り返す";
    EXPECT_GT(height(left), wide_height);

    measurer_.MeasureNode(nodes[0], entry, 800.0f);
    EXPECT_GE(left->GetMaxWidth(), 1.0e6f);
    EXPECT_FLOAT_EQ(height(left), wide_height);
}

// リロードで引き継いだ layout は、旧キャッシュで持ちうる範囲 (前回の keep・その後の計測) だけを新しいキャッシュに記録する。
// 記録した範囲は新しいキャッシュの最初の evict で keep の外なら捨て、記録しなかった範囲はなめない。
TEST_F(DWriteMeasurerTest, CarriedLayoutsAreEvictedOnFirstEvict)
{
    std::string md;
    for (int i = 0; i < 10; ++i) {
        md += std::format("para {}\n\n", i);
    }
    auto [old_nodes, old_cache] = ParseAndLayout(md);
    ASSERT_EQ(old_nodes.size(), 10u);
    // 旧キャッシュは [6, 10) を keep にして evict 済み。layout は 6〜9 だけに残る。
    old_cache.EvictTextLayouts(6, 10);
    ASSERT_EQ(old_cache[5].text_layout(), nullptr);
    ASSERT_NE(old_cache[6].text_layout(), nullptr);

    auto [new_nodes, new_cache] = ParseAndLayout(md, 800.0f, 0.0f, 0.0f);
    new_cache = layout::MakeEstimatedLayoutCache(new_nodes, theme_, 800.0f);
    layout::CarryOverUnchanged(old_nodes, old_cache, new_nodes, new_cache, FindUnchangedEnds(old_nodes, new_nodes));
    ASSERT_NE(new_cache[6].text_layout(), nullptr);

    new_cache.EvictTextLayouts(8, 10);
    EXPECT_EQ(new_cache[6].text_layout(), nullptr);
    EXPECT_EQ(new_cache[7].text_layout(), nullptr);
    EXPECT_NE(new_cache[8].text_layout(), nullptr);
    EXPECT_NE(new_cache[9].text_layout(), nullptr);
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
        std::vector<DWRITE_HIT_TEST_METRICS> m;
        const UINT32 count = FetchHitTestMetrics(layout, start, length, m);
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
    EXPECT_EQ(requests[0], InlineMathKey("x^2", theme_.font_size_body));

    table_.Resolve(requests[0], { 123.0f, 30.0f, 20.0f });
    EXPECT_TRUE(cache.InvalidatePendingMathLayouts(true));
    EXPECT_FALSE(cache.InvalidatePendingMathLayouts(true)) << "印は 1 回で外す";
    ComputeLayout(engine_, nodes, cache, 800.0f);
    EXPECT_FALSE(cache[0].pending_math);
    EXPECT_TRUE(table_.TakeRequests().empty());
    EXPECT_FLOAT_EQ(RangeWidth(cache[0].text_layout(), 2, 5), 123.0f);
}

// 画面外の evict で layout を捨てたら、組み直しを待つ印も外す。寸法が届いても組み直す相手が無い。
TEST_F(DWriteInlineMathTest, EvictingLayoutClearsPendingMath)
{
    auto [nodes, cache] = ParseAndLayout("a $x^2$ b");
    ASSERT_TRUE(cache[0].pending_math);
    cache.NoteMaterialized(0, 1);
    cache.EvictTextLayouts(1, 1);
    EXPECT_EQ(cache[0].text_layout(), nullptr);
    EXPECT_FALSE(cache[0].pending_math);
    EXPECT_FALSE(cache.InvalidatePendingMathLayouts(true));
}

// 数式の途中や頭だけを指す範囲 (数式の中の検索ヒット) も、数式全体を含む範囲に広げてから箱を引く。
// 広げないと HitTestTextRange は幅 0 の箱や、数式を除いた箱を返す。
TEST_F(DWriteInlineMathTest, ExpandToClustersCoversWholeFormula)
{
    auto [nodes, cache] = ParseAndLayout("a $E = mc^2$ b");
    auto requests = table_.TakeRequests();
    ASSERT_EQ(requests.size(), 1u);
    table_.Resolve(requests[0], { 60.0f, 24.0f, 18.0f });
    cache.InvalidatePendingMathLayouts(true);
    ComputeLayout(engine_, nodes, cache, 800.0f);
    auto* layout = cache[0].text_layout();
    ASSERT_NE(layout, nullptr);
    EXPECT_FLOAT_EQ(RangeWidth(layout, 7, 2), 0.0f) << "広げないと幅 0";

    struct Case {
        UINT32 start;
        UINT32 length;
        UINT32 expanded_start;
        UINT32 expanded_end;
    };
    for (const auto& c : { Case{ 7, 2, 2, 12 }, Case{ 1, 3, 1, 12 }, Case{ 10, 4, 2, 14 }, Case{ 0, 1, 0, 1 } }) {
        SCOPED_TRACE(std::format("{}+{}", c.start, c.length));
        const auto r = ExpandToClusters(layout, c.start, c.length);
        EXPECT_EQ(r.startPosition, c.expanded_start);
        EXPECT_EQ(r.startPosition + r.length, c.expanded_end);
    }
    const auto inside = ExpandToClusters(layout, 7, 2);
    EXPECT_FLOAT_EQ(RangeWidth(layout, inside.startPosition, inside.length), 60.0f);
}

// $x$ と文中の $$x$$ は同じ形式で描くので、同じ数式として 1 回だけ頼む。
TEST_F(DWriteInlineMathTest, InlineAndDisplayMathShareRendering)
{
    auto [nodes, cache] = ParseAndLayout("a $x$ b $$x$$ c\n");
    ASSERT_EQ(nodes.size(), 1u);
    const auto requests = table_.TakeRequests();
    ASSERT_EQ(requests.size(), 1u);
    EXPECT_EQ(requests[0], InlineMathKey("x", theme_.font_size_body));
}

// $$..$$ だけの段落 (引用の中も、```math も) は中央に置く。文中の $$..$$ は左揃えのまま。
TEST_F(DWriteInlineMathTest, BlockMathIsCentered)
{
    auto [nodes, cache] = ParseAndLayout("$$w$$\n\n> $$y=x$$\n\na $$z$$ b\n\n```math\na\nb\n```\n");
    ASSERT_EQ(nodes.size(), 4u);
    auto requests = table_.TakeRequests();
    std::vector<uint64_t> expected;
    for (const std::string_view tex : { "a\nb", "w", "y=x", "z" }) {
        expected.push_back(InlineMathKey(tex, theme_.font_size_body));
    }
    std::ranges::sort(requests);
    std::ranges::sort(expected);
    EXPECT_EQ(requests, expected);
    EXPECT_EQ(cache[0].text_layout()->GetTextAlignment(), DWRITE_TEXT_ALIGNMENT_CENTER);
    EXPECT_EQ(cache[1].text_layout()->GetTextAlignment(), DWRITE_TEXT_ALIGNMENT_CENTER);
    EXPECT_EQ(cache[2].text_layout()->GetTextAlignment(), DWRITE_TEXT_ALIGNMENT_LEADING);
    EXPECT_EQ(cache[3].text_layout()->GetTextAlignment(), DWRITE_TEXT_ALIGNMENT_CENTER);
}

// ブロック数式は幅に収まらなければ幅いっぱいに縮め、幅が変わったら組み直して倍率も変える。文中の数式は縮めない。
TEST_F(DWriteInlineMathTest, WideBlockMathShrinksToWidth)
{
    auto [nodes, cache] = ParseAndLayout("$$x$$\n\na $$x$$ b\n");
    ASSERT_EQ(nodes.size(), 2u);
    auto requests = table_.TakeRequests();
    ASSERT_EQ(requests.size(), 1u);
    table_.Resolve(requests[0], { 2000.0f, 40.0f, 30.0f });
    cache.InvalidatePendingMathLayouts(true);
    for (const float viewport : { 800.0f, 600.0f }) {
        SCOPED_TRACE(viewport);
        ComputeLayout(engine_, nodes, cache, viewport);
        auto* block = cache[0].text_layout();
        ASSERT_NE(block, nullptr);
        EXPECT_NEAR(RangeWidth(block, 0, 5), block->GetMaxWidth(), 0.01f);
        EXPECT_FLOAT_EQ(RangeWidth(cache[1].text_layout(), 2, 5), 2000.0f);
    }
}

// 区切りのそろった run だけを数式にする。長い本文を区切って片側の $$ が無い run は原文のまま組む。
TEST(InlineMathTex, ExtractsTexAndRejectsUnbalancedRuns)
{
    const auto make = [](uint32_t start, uint32_t length, uint8_t flags) static {
        return TextRun{ .start = start, .length = length, .flags = flags };
    };
    EXPECT_EQ(InlineMathTex("a $x$ b", make(2, 3, TextRun::kMath)), std::optional<std::string_view>{ "x" });
    EXPECT_EQ(InlineMathTex("$$x+y$$", make(0, 7, TextRun::kMathDoubleDollar)), std::optional<std::string_view>{ "x+y" });
    EXPECT_FALSE(InlineMathTex("$$x+y", make(0, 5, TextRun::kMathDoubleDollar)));
    EXPECT_FALSE(InlineMathTex("x+y$$", make(0, 5, TextRun::kMathDoubleDollar)));
    EXPECT_FALSE(InlineMathTex("$$$$", make(0, 4, TextRun::kMathDoubleDollar)));
}

// 長くて区切った $$..$$ は数式にしない (どのノードも原文のまま)。
TEST_F(DWriteInlineMathTest, SplitDisplayMathStaysAsText)
{
    std::string md = RepeatTo("x + ", kTextChunkTarget * 2, "a $$");
    md += "y$$ b";
    auto [nodes, cache] = ParseAndLayout(md);
    ASSERT_GT(nodes.size(), 1u);
    EXPECT_TRUE(table_.TakeRequests().empty());
    for (size_t i = 0; i < nodes.size(); ++i) {
        EXPECT_FALSE(cache[i].pending_math);
    }
}

// 描けなかった数式は待たずに原文のまま組む。
TEST_F(DWriteInlineMathTest, FailedMathStaysAsText)
{
    const uint64_t key = InlineMathKey(R"(\frac{a})", theme_.font_size_body);
    table_.FindOrRequest(key);
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
    std::vector<uint64_t> expected{ InlineMathKey("x", theme_.GetHeadingSize(1)), InlineMathKey("x", theme_.font_size_body),
                                    InlineMathKey("y", theme_.font_size_body) };
    std::ranges::sort(requests);
    std::ranges::sort(expected);
    EXPECT_EQ(requests, expected);
    const auto* tl = cache[2].table_layout();
    ASSERT_NE(tl, nullptr);
    EXPECT_TRUE(cache[2].pending_math);
    // 表を待たせても、段落は組み直す。
    EXPECT_TRUE(cache.InvalidatePendingMathLayouts(false));
    EXPECT_EQ(cache[2].table_layout(), tl);
    EXPECT_TRUE(cache.InvalidatePendingMathLayouts(true));
    EXPECT_EQ(cache[2].table_layout(), nullptr);
    EXPECT_TRUE(cache[2].layout_dirty);
}

// キーの大きさ (MathFontSize) は組んだ文字の大きさと同じ。ずれると数式の大きさが周りの文字と合わない。
TEST_F(DWriteInlineMathTest, MathFontSizeMatchesLaidOutText)
{
    auto [nodes, cache] = ParseAndLayout("# $a$\n\n## $a$\n\n### $a$\n\n#### $a$\n\n##### $a$\n\n###### $a$\n\n"
                                         "$a$\n\n> $a$\n\n- $a$\n\n| $a$ |\n|---|\n| $a$ |\n");
    const auto laid_out_size = [](IDWriteTextLayout* layout) {
        FLOAT size = 0.0f;
        EXPECT_HRESULT_SUCCEEDED(layout->GetFontSize(0, &size));
        return size;
    };
    for (size_t i = 0; i < nodes.size(); ++i) {
        SCOPED_TRACE(i);
        if (const auto* table = nodes[i].table_data()) {
            const auto* tl = cache[i].table_layout();
            ASSERT_NE(tl, nullptr);
            for (size_t r = 0; r < table->row_count; ++r) {
                EXPECT_FLOAT_EQ(laid_out_size(tl->GetCellLayout(r, 0)), MathFontSize(theme_, *table, r));
            }
            continue;
        }
        ASSERT_NE(cache[i].text_layout(), nullptr);
        EXPECT_FLOAT_EQ(laid_out_size(cache[i].text_layout()), MathFontSize(theme_, nodes[i]));
    }
}

// 計測と計画はノードと run を別々にたどる。ずれると計測が頼んだ数式が計画に無く、描かれない。
TEST_F(DWriteInlineMathTest, PlanCoversEveryMeasuredMath)
{
    // 計画はパーサーが集めた数式のあるノードだけを見るので、その番号の漏れも捕まえる。
    constexpr std::string_view kMd = "# $a$\n\n## $a$\n\n### $a$\n\n#### $a$\n\n##### $a$\n\n###### $a$\n\n"
                                     "$a$ **$b$** `c` $$d$$\n\n> $e$\n\n- $f$\n\n| $g$ |\n|---|\n| $h$ |\n";
    auto [nodes, cache] = ParseAndLayout(kMd);
    const auto requests = table_.TakeRequests();
    InlineMathPlan plan;
    plan.Build(nodes, ParseNodesWithMath(kMd).math_indices, theme_);
    size_t planned = 0;
    for (const auto& c : plan.Chunks()) {
        planned += c.items.size();
    }
    EXPECT_EQ(requests.size(), planned);
    for (const uint64_t key : requests) {
        EXPECT_TRUE(plan.ChunkOf(key)) << key;
    }
}

// 同じ数式は結果が出るまで 1 回だけ頼み、溜まり始めたときだけ知らせる。
TEST(InlineMathTable, RequestsOnceAndWakesWhenQueueStarts)
{
    InlineMathTable table;
    int wakes = 0;
    table.SetWake([&] { ++wakes; });
    EXPECT_FALSE(table.FindOrRequest(1));
    EXPECT_FALSE(table.FindOrRequest(1));
    EXPECT_FALSE(table.FindOrRequest(2));
    EXPECT_EQ(wakes, 1);
    EXPECT_EQ(table.TakeRequests().size(), 2u);

    EXPECT_TRUE(table.HasUnresolved());
    table.Resolve(1, { 10.0f, 5.0f, 4.0f });
    const auto found = table.FindOrRequest(1);
    ASSERT_TRUE(found);
    EXPECT_FLOAT_EQ(found->width, 10.0f);
    EXPECT_TRUE(table.HasUnresolved());
    table.Resolve(2, {});
    EXPECT_FALSE(table.HasUnresolved()) << "描けなかった数式も結果が出たら待ちから外す";
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
    const auto [nodes, math] = ParseNodesWithMath("# $h$\n\n$a$ $b$ $a$\n\n| $t$ |\n|---|\n| $c$ |\n");
    InlineMathPlan plan;
    plan.Build(nodes, math, theme);
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
    again.Build(nodes, math, theme);
    EXPECT_EQ(ChunkKeys(again), ChunkKeys(plan));
}

// 組は結果を待つ間は取り出せず、描けたらまた取り出せる。
TEST(InlineMathPlan, TakesChunkAgainAfterFinished)
{
    InlineMathPlan plan;
    const auto parsed = ParseNodesWithMath("$a$ $b$\n");
    plan.Build(parsed.nodes, parsed.math_indices, GetLightTheme());
    ASSERT_EQ(plan.Chunks().size(), 1u);
    const uint64_t key = plan.Chunks()[0].key;
    EXPECT_EQ(plan.TakeChunk(0), plan.Chunks().data());
    EXPECT_EQ(plan.TakeChunk(0), nullptr);
    plan.FinishChunk(0, key);
    EXPECT_EQ(plan.TakeChunk(0), plan.Chunks().data());
}

// 計画を作り直す前に頼んだ組 (番号か key が合わない) の結果は状態を変えない。
TEST(InlineMathPlan, StaleChunksDoNotFinish)
{
    InlineMathPlan plan;
    const auto [nodes, math] = ParseNodesWithMath("$a$\n");
    plan.Build(nodes, math, GetLightTheme());
    const uint64_t key = plan.Chunks()[0].key;
    ASSERT_NE(plan.TakeChunk(0), nullptr);
    plan.FinishChunk(0, key + 1);
    plan.FinishChunk(1, key);
    EXPECT_EQ(plan.TakeChunk(0), nullptr) << "まだ待っている";
    plan.FinishChunk(0, key);
    EXPECT_NE(plan.TakeChunk(0), nullptr);
}

// 文書の順に kInlineMathPerChunk 個ずつ区切る。最後の組は残りの数式。
TEST(InlineMathPlan, SplitsInDocumentOrder)
{
    const Theme theme = GetLightTheme();
    std::string text;
    const size_t count = kInlineMathPerChunk * 2 + 10;
    for (size_t i = 0; i < count; ++i) {
        text += std::format("${}x$ ", i);
    }
    InlineMathPlan plan;
    const auto parsed = ParseNodesWithMath(text);
    plan.Build(parsed.nodes, parsed.math_indices, theme);
    const auto chunks = plan.Chunks();
    ASSERT_EQ(chunks.size(), 3u);
    EXPECT_EQ(chunks[0].items.size(), kInlineMathPerChunk);
    EXPECT_EQ(chunks[1].items.size(), kInlineMathPerChunk);
    EXPECT_EQ(chunks[2].items.size(), 10u);
    size_t n = 0;
    for (const auto& c : chunks) {
        for (const auto& item : c.items) {
            EXPECT_EQ(item.tex, std::format("{}x", n++));
        }
    }
}

// 組ごと描くとき、まだ測っていない数式も頼んだことにして、計測から二重に頼ませない。描けた数式は上書きしない。
TEST(InlineMathTable, MarkRequestedSuppressesRequestsFromMeasuring)
{
    InlineMathTable table;
    table.FindOrRequest(1);
    table.Resolve(1, { 10.0f, 5.0f, 4.0f });
    table.TakeRequests();
    const InlineMathItem items[]{ { 1, "a", 16.0f }, { 2, "b", 16.0f } };
    table.MarkRequested(items);
    EXPECT_TRUE(table.FindOrRequest(1));
    EXPECT_FALSE(table.FindOrRequest(2));
    EXPECT_TRUE(table.TakeRequests().empty());
    EXPECT_TRUE(table.Resolve(2, { 10.0f, 5.0f, 4.0f }));
}

// Clear の前に頼んだ結果が後から届いても表に戻さず、呼び出し側に型を置かせない。
TEST(InlineMathTable, ResolveAfterClearIsIgnored)
{
    InlineMathTable table;
    table.FindOrRequest(1);
    table.Clear();
    EXPECT_TRUE(table.TakeRequests().empty());
    EXPECT_FALSE(table.Resolve(1, { 10.0f, 5.0f, 4.0f }));
    EXPECT_FALSE(table.FindOrRequest(1)) << "忘れたので頼み直す";
    EXPECT_TRUE(table.Resolve(1, { 10.0f, 5.0f, 4.0f }));
}

// 頼まなかったことにした数式は待ちから外れ、また測られたら頼み直す。結果が分かっている数式は残す。
TEST(InlineMathTable, ForgetRequestDropsOnlyWaitingEntry)
{
    InlineMathTable table;
    table.FindOrRequest(1);
    table.FindOrRequest(2);
    table.TakeRequests();
    table.Resolve(2, { 10.0f, 5.0f, 4.0f });

    table.ForgetRequest(1);
    table.ForgetRequest(2);
    table.ForgetRequest(3);
    EXPECT_FALSE(table.HasUnresolved());
    EXPECT_TRUE(table.FindOrRequest(2));

    EXPECT_FALSE(table.FindOrRequest(1));
    EXPECT_EQ(table.TakeRequests(), (std::vector<uint64_t>{ 1 }));
}

// MockTextMeasurer と DWriteTextMeasurer の不変条件を突き合わせる回帰テスト。
// 絶対値は両者で異なる（mock はヒューリスティック、DWrite は実測）が、
// 「どちらも正の高さを返す」「順序性を維持する」「同じスケーリング式を使う」等の
// 共通ルールを検証することで、片方だけ仕様が変わった際の検出ポイントを残す。
class MeasurerParityTest : public DWriteTestBase {
protected:
    MockTextMeasurer mock_;

    void SetUp() override
    {
        DWriteTestBase::SetUp();
        mock_.SetTheme(theme_);
    }

    struct Heights {
        float mock;
        float dwrite;
    };

    // 本番と同じく MeasureEntry 経由で計測する。
    Heights MeasureBoth(const Node& node, float max_width)
    {
        NodeLayoutEntry mock_entry{};
        NodeLayoutEntry dwrite_entry{};
        MeasureEntry(mock_, node, mock_entry, max_width, nullptr);
        MeasureEntry(measurer_, node, dwrite_entry, max_width, nullptr);
        return { mock_entry.height, dwrite_entry.height };
    }
};

// テキストの無い水平線と空の段落も、両計測とも正の高さを返すこと (空の段落はマージン確保のため)。
TEST_F(MeasurerParityTest, TextlessNodesArePositive)
{
    for (const NodeType type : { NodeType::HorizontalRule, NodeType::Paragraph }) {
        SCOPED_TRACE(static_cast<int>(type));
        Node node;
        node.type = type;
        const auto h = MeasureBoth(node, 600.0f);
        EXPECT_GT(h.mock, 0.0f);
        EXPECT_GT(h.dwrite, 0.0f);
    }
}

// 見出しと段落の順序性: 同じテキストに対し、両計測とも見出し > 段落 となること。
TEST_F(MeasurerParityTest, HeadingTallerThanParagraphInBoth)
{
    Node para = MakeTextNode("Sample text");

    Node heading = MakeTextNode("Sample text");
    heading.type = NodeType::Heading;
    heading.heading_level = 1;

    const auto p = MeasureBoth(para, 600.0f);
    const auto h = MeasureBoth(heading, 600.0f);

    EXPECT_GT(p.mock, 0.0f);
    EXPECT_GT(p.dwrite, 0.0f);
    EXPECT_GT(h.mock, p.mock);
    EXPECT_GT(h.dwrite, p.dwrite);
}
