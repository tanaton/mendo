#include <gtest/gtest.h>
#include "mermaid_util.h"
#include "parser.h"
#include "test_nodes.h"
#include <algorithm>
#include <vector>

using namespace std::literals;

// 32 の倍数へ切り上げ、倍数はそのまま。0 以下は最小の 32。
TEST(QuantizeWidth, RoundsUpToMultipleOf32)
{
    struct Case {
        float width;
        int expected;
    };
    constexpr Case kCases[] = {
        { 750.0f, 768 },
        { 801.0f, 832 },
        { 1.0f, 32 },
        { 31.0f, 32 },
        { 1920.0f, 1920 },
        { 1921.0f, 1952 },
        { 32.0f, 32 },
        { 64.0f, 64 },
        { 800.0f, 800 },
        { 1024.0f, 1024 },
        { 0.0f, 32 },
        { -1.0f, 32 },
        { -100.0f, 32 },
    };
    for (const auto& c : kCases) {
        SCOPED_TRACE(c.width);
        EXPECT_EQ(mermaid_util::QuantizeWidth(c.width), c.expected);
    }
}

// 描画命令の中身のどれが違ってもハッシュが変わる (別の幅で描いた図が同じかの判定)。ラベルの数式の描画命令は見ない。
TEST(DrawingHash, DependsOnEveryPart)
{
    using Drawing = mermaid_util::DiagramDrawing;
    const auto make = [] {
        Drawing d{ .width = 10.0f, .height = 20.0f, .background = 0xFFFFFF, .defs = "<g/>" };
        auto& layer = d.layers.emplace_back();
        layer.shapes = "<rect/>";
        layer.texts.push_back({ .at = { .font_size = 16.0f }, .family = mermaid_util::AppendChars(d.chars, L"Segoe UI"), .text = mermaid_util::AppendChars(d.chars, L"x") });
        layer.maths.push_back({ .at = { .font_size = 16.0f }, .tex = "y" });
        return d;
    };
    const uint64_t base = mermaid_util::DrawingHash(make());
    EXPECT_EQ(mermaid_util::DrawingHash(make()), base);
    void (*const kEdits[])(Drawing&) = {
        [](Drawing& d) { d.width = 11.0f; },
        [](Drawing& d) { d.height = 21.0f; },
        [](Drawing& d) { d.background = 0; },
        [](Drawing& d) { d.defs = "<g />"; },
        [](Drawing& d) { d.chars.back() = L'z'; },
        [](Drawing& d) { d.layers[0].shapes = "<path/>"; },
        [](Drawing& d) { d.layers[0].texts[0].at.x = 1.0f; },
        [](Drawing& d) { d.layers[0].texts[0].weight = 700; },
        [](Drawing& d) { d.layers[0].texts[0].italic = true; },
        [](Drawing& d) { d.layers[0].texts[0].decoration = mermaid_util::kTextUnderline; },
        [](Drawing& d) { d.layers[0].texts[0].family.length = 1; },
        [](Drawing& d) { d.layers[0].texts[0].text.length = 0; },
        [](Drawing& d) { d.layers[0].texts.clear(); },
        [](Drawing& d) { d.layers[0].maths.clear(); },
        [](Drawing& d) { d.layers.emplace_back(); },
        [](Drawing& d) { d.layers[0].maths[0].at.y = 1.0f; },
        [](Drawing& d) { d.layers[0].maths[0].tex = "z"; },
    };
    for (const auto edit : kEdits) {
        Drawing d = make();
        edit(d);
        EXPECT_NE(mermaid_util::DrawingHash(d), base);
    }
}

// どの幅でも使える結果にしてよいのはフローチャートだけ。前置き・コメント・空行は読み飛ばす。
TEST(IsFlowchartCode, DetectsFlowchartAfterPreamble)
{
    EXPECT_TRUE(mermaid_util::IsFlowchartCode("flowchart LR\n  A --> B\n"));
    EXPECT_TRUE(mermaid_util::IsFlowchartCode("graph TD;A-->B"));
    EXPECT_TRUE(mermaid_util::IsFlowchartCode("---\ntitle: x\n---\n%% comment\n\n  flowchart TB\n"));
    EXPECT_FALSE(mermaid_util::IsFlowchartCode("sequenceDiagram\n  A->>B: hi\n"));
    EXPECT_FALSE(mermaid_util::IsFlowchartCode("gantt\n  title flowchart\n"));
    EXPECT_FALSE(mermaid_util::IsFlowchartCode("---\nflowchart: x\n---\nclassDiagram\n"));
    EXPECT_FALSE(mermaid_util::IsFlowchartCode(""));
}

TEST(SanitizeErrorMessage, CollapsesTrimsAndTruncates)
{
    struct Case {
        std::wstring_view msg;
        size_t max_len;
        std::wstring_view expected;
    };
    constexpr Case kCases[] = {
        { L""sv, 100, L""sv },
        { L"simple error"sv, 100, L"simple error"sv },
        { L"Parse error on line 2:\n\n----^\t got 'X'"sv, 100, L"Parse error on line 2: ----^ got 'X'"sv },
        // pele の文言に入る制御文字も豆腐表示にせず空白に潰す
        { L"got\x0b\x1bhere"sv, 100, L"got here"sv },
        { L"  \n boom \n "sv, 100, L"boom"sv },
        { L" \n\t "sv, 100, L""sv },
        { L"abcdefghij"sv, 5, L"abcd…"sv },
        { L"abcde"sv, 5, L"abcde"sv },
        // max_len 到達後の残りが空白のみなら実質切り詰め無しで省略記号を付けない
        { L"abcde \n\t "sv, 5, L"abcde"sv },
    };
    for (const auto& c : kCases) {
        SCOPED_TRACE(::testing::PrintToString(std::wstring{ c.msg }) + " max_len=" + std::to_string(c.max_len));
        EXPECT_EQ(mermaid_util::SanitizeErrorMessage(c.msg, c.max_len), c.expected);
    }
}

namespace {

Node ParseSingleCodeBlock(std::string_view md)
{
    auto nodes = ParseNodes(md);
    EXPECT_EQ(nodes.size(), 1u);
    return std::move(nodes.front());
}

uint64_t RequestHash(const Node& node, float max_width, bool dark_mode)
{
    return mermaid_util::DiagramRequestHash(mermaid_util::DiagramCodeHash(node), max_width, dark_mode);
}

} // namespace

TEST(DiagramRequestHash, DependsOnTextWidthBucketAndTheme)
{
    const auto a = ParseSingleCodeBlock("```mermaid\ngraph TD; A-->B\n```");
    const auto b = ParseSingleCodeBlock("```mermaid\ngraph TD; A-->C\n```");
    const uint64_t base = RequestHash(a, 600.0f, false);

    EXPECT_NE(RequestHash(b, 600.0f, false), base);
    EXPECT_NE(RequestHash(a, 600.0f, true), base);
    EXPECT_NE(RequestHash(a, 900.0f, false), base);
}

// 幅は 32 刻みで量子化されるため、同じバケット内のリサイズではキャッシュを再利用する。
TEST(DiagramRequestHash, SameWidthBucketSharesHash)
{
    const auto a = ParseSingleCodeBlock("```mermaid\ngraph TD; A-->B\n```");
    ASSERT_EQ(mermaid_util::QuantizeWidth(590.0f), mermaid_util::QuantizeWidth(605.0f));
    EXPECT_EQ(RequestHash(a, 590.0f, false), RequestHash(a, 605.0f, false));
}

// 幅の上限に収まる図の大きさはそのまま。収まらない図は幅を上限に合わせ、ブラウザのレイアウトと同じく 1/64 px に切り捨てて
// から切り上げる (保存・コピーの画像の大きさ)。
TEST(FittedDiagramSize, KeepsFittingSizeAndFloorsShrunkSizeTo64ths)
{
    const mermaid_util::DiagramDrawing drawing{ .width = 837.0f, .height = 520.0f };
    for (const float fits : { 837.0f, 900.0f, 0.0f }) {
        const auto size = mermaid_util::FittedDiagramSize(drawing, fits);
        EXPECT_EQ(size.width, 837.0f) << fits;
        EXPECT_EQ(size.height, 520.0f) << fits;
    }
    // 520 * 800 / 837 = 497.013 → 497 (切り上げるだけだと 498)。
    const auto shrunk = mermaid_util::FittedDiagramSize(drawing, 800.0f);
    EXPECT_EQ(shrunk.width, 800.0f);
    EXPECT_EQ(shrunk.height, 497.0f);
    const auto fractional = mermaid_util::FittedDiagramSize(drawing, 413.5f);
    EXPECT_EQ(fractional.width, 414.0f);
    EXPECT_EQ(fractional.height, 257.0f);
}

// 幅を混ぜないキーは、どの幅のキーとも重ならず、テーマでは分かれる。
TEST(DiagramWidthFreeHash, DistinctFromWidthKeys)
{
    const auto a = ParseSingleCodeBlock("```mermaid\ngraph TD; A-->B\n```");
    const uint64_t code = mermaid_util::DiagramCodeHash(a);
    const uint64_t free_light = mermaid_util::DiagramWidthFreeHash(code, false);
    EXPECT_NE(mermaid_util::DiagramWidthFreeHash(code, true), free_light);
    for (const float w : { 0.0f, 1.0f, 32.0f, 600.0f, 4000.0f }) {
        EXPECT_NE(mermaid_util::DiagramRequestHash(code, w, false), free_light) << w;
    }
}

// 1 DIP の余裕を見て、幅の上限より狭い図だけを幅によらない結果として扱う。
TEST(DiagramWidthFreeHash, IsWidthFreeNeedsMargin)
{
    EXPECT_TRUE(mermaid_util::IsWidthFree(400.0f, 600.0f));
    EXPECT_FALSE(mermaid_util::IsWidthFree(599.5f, 600.0f));
    EXPECT_FALSE(mermaid_util::IsWidthFree(600.0f, 600.0f));
}
