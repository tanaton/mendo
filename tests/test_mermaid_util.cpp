#include <gtest/gtest.h>
#include "mermaid_util.h"
#include "parser.h"
#include "string_convert.h"
#include "test_nodes.h"
#include "utility.h"
#include <algorithm>
#include <vector>

using namespace std::literals;

// ═══════════════════════════════════════════════
// QuantizeWidth
// ═══════════════════════════════════════════════

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

// 文字の並びの本文と書体、数式の TeX は長さで切るので、区切りの ':' や ',' を含んでもそのまま読める。
TEST(ParseDrawing, ParsesShapesTextRunsAndMaths)
{
    const auto d = mermaid_util::ParseDrawing(
        L"400:300.5:2039583:8:<g>い</g>2:"
        L"12:<svg>あ</svg>2:"
        L"1,0,0,1,10.5,20,3,4.25,16,16711680,0.5,700,1,0:10:Segoe UI,x5:a:b,c"
        L"0,1,-1,0,0,0,0,0,12,0,1,400,0,1:0:0:"
        L"1:1,0,0,1,0,0,7,8.5,14,255,0.75:9:x_{a:b},c"
        L"0:0:0:"sv);
    ASSERT_TRUE(d.has_value());
    EXPECT_EQ(d->width, 400.0f);
    EXPECT_EQ(d->height, 300.5f);
    EXPECT_EQ(d->background, 0x1F1F1Fu);
    EXPECT_EQ(d->defs, "<g>\xE3\x81\x84</g>");
    ASSERT_EQ(d->layers.size(), 2u);
    EXPECT_EQ(d->layers[0].shapes, "<svg>\xE3\x81\x82</svg>");
    EXPECT_TRUE(d->layers[1].shapes.empty());
    EXPECT_TRUE(d->layers[1].texts.empty());
    const auto& texts = d->layers[0].texts;
    ASSERT_EQ(texts.size(), 2u);
    const auto& t = texts[0];
    EXPECT_EQ(t.at.transform, (std::array{ 1.0f, 0.0f, 0.0f, 1.0f, 10.5f, 20.0f }));
    EXPECT_EQ(t.at.x, 3.0f);
    EXPECT_EQ(t.at.y, 4.25f);
    EXPECT_EQ(t.at.font_size, 16.0f);
    EXPECT_EQ(t.weight, 700);
    EXPECT_TRUE(t.italic);
    EXPECT_FALSE(t.underline);
    EXPECT_EQ(t.at.rgb, 0xFF0000u);
    EXPECT_EQ(t.at.alpha, 0.5f);
    EXPECT_EQ(t.family, L"Segoe UI,x"sv);
    EXPECT_EQ(t.text, L"a:b,c"sv);
    EXPECT_TRUE(texts[1].underline);
    EXPECT_TRUE(texts[1].family.empty());
    EXPECT_TRUE(texts[1].text.empty());
    ASSERT_EQ(d->layers[0].maths.size(), 1u);
    const auto& m = d->layers[0].maths[0];
    EXPECT_EQ(m.at.x, 7.0f);
    EXPECT_EQ(m.at.y, 8.5f);
    EXPECT_EQ(m.at.font_size, 14.0f);
    EXPECT_EQ(m.at.rgb, 0xFFu);
    EXPECT_EQ(m.at.alpha, 0.75f);
    EXPECT_EQ(m.tex, "x_{a:b},c");
    EXPECT_EQ(m.drawing, nullptr) << "描画命令は読んだ後に組み直す";
    EXPECT_TRUE(d->layers[1].maths.empty());
}

// 数式も書いた形のまま読み戻せる (TeX の BMP 外の文字は UTF-16 で 2 単位に数える)。
TEST(ParseDrawing, SerializedMathsParseBack)
{
    mermaid_util::DiagramDrawing drawing{ .width = 10.0f, .height = 20.0f, .background = 0xFFFFFF, .defs = "<linearGradient id=\"g0\"/>" };
    drawing.layers.push_back({ .maths = { { .at = { .transform = { 2.0f, 0.0f, 0.0f, 2.0f, 1.0f, 2.0f },
                                                    .x = 0.5f,
                                                    .y = 12.25f,
                                                    .font_size = 16.0f,
                                                    .rgb = 0x123456,
                                                    .alpha = 1.0f },
                                            .tex = "\\mathcal{A}\xF0\x9D\x91\xA5" } } });
    const auto back = mermaid_util::ParseDrawing(string_convert::Utf8ToWide(mermaid_util::SerializeDrawing(drawing)));
    ASSERT_TRUE(back);
    EXPECT_EQ(*back, drawing);
}

// 書体と TeX は長くても読み戻せる (図を作る側に長さの上限は無い)。
TEST(ParseDrawing, LongFamilyAndTexParseBack)
{
    mermaid_util::DiagramDrawing drawing{ .width = 10.0f, .height = 20.0f };
    drawing.layers.push_back({
        .texts = { { .at = { .font_size = 16.0f }, .family = std::wstring(2000, L'a'), .text = L"x" } },
        .maths = { { .at = { .font_size = 16.0f }, .tex = std::string(70000, 'x') } },
    });
    const auto back = mermaid_util::ParseDrawing(string_convert::Utf8ToWide(mermaid_util::SerializeDrawing(drawing)));
    ASSERT_TRUE(back);
    EXPECT_EQ(*back, drawing);
}

// ファイルは壊れていることがあるので、欠け・余り・ありえない値は全体を捨てる。
TEST(ParseDrawing, RejectsMalformedPayloads)
{
    constexpr std::wstring_view kRun = L"1,0,0,1,0,0,0,0,16,0,1,400,0,0:0:1:a"sv;
    const auto with_run = [&](std::wstring_view run) { return std::wstring{ L"10:20:0:0:1:0:1:" } + std::wstring{ run } + L"0:"; };
    const auto with_math = [&](std::wstring_view math) { return std::wstring{ L"10:20:0:0:1:0:0:1:" } + std::wstring{ math }; };
    EXPECT_TRUE(mermaid_util::ParseDrawing(with_run(kRun)).has_value());
    EXPECT_TRUE(mermaid_util::ParseDrawing(with_math(L"1,0,0,1,0,0,0,0,16,0,1:1:x")).has_value());
    const std::wstring kBad[] = {
        L"",
        L"10:x",                          // 高さが無い
        L"nan:20:0:0:1:0:0:0:",             // 幅が数でない
        L"inf:20:0:0:1:0:0:0:",           // 幅が有限でない
        L"0:20:0:0:1:0:0:0:",             // 幅が 0
        L"10:20:16777216:0:1:0:0:0:",     // 背景が 24 ビットを超える
        L"10:20:0:5:<g/>1:0:0:0:",        // defs の長さが合わない
        L"10:20:0:1:0:0:0:",              // defs が無い
        L"10:20:0:0:1:9:<svg>0:0:",       // SVG の長さが足りない
        L"10:20:0:0:1:0:0:0:x",           // 余り
        L"10:20:0:0:2:0:0:0:",            // 層が足りない
        L"10:20:0:0:1:0:0:",              // 数式の数が無い
        L"10:20:0:0:1:0:2:" + std::wstring{ kRun } + L"0:", // 並びが足りない
        with_math(L""),                                  // 数式が足りない
        with_math(L"1,0,0,1,0,0,0,0,16,0:1:x"),           // 数式の数が 10 個
        with_math(L"1,0,0,1,0,0,0,0,0,0,1:1:x"),          // 数式の大きさが 0
        with_math(L"inf,0,0,1,0,0,0,0,16,0,1:1:x"),       // 数式の変換が有限でない
        with_math(L"1,0,0,1,0,0,0,0,16,16777216,1:1:x"),  // 数式の色が 24 ビットを超える
        with_math(L"1,0,0,1,0,0,0,0,16,0,1:2:x"),         // TeX の長さが足りない
        with_run(L"1,0,0,1,0,0,0,0,16,400,0,0,0:0:1:a"),        // 数が 13 個
        with_run(L"1,0,0,1,0,0,0,0,0,0,1,400,0,0:0:1:a"),       // 大きさが 0
        with_run(L"1,0,0,1,0,0,0,0,1001,0,1,400,0,0:0:1:a"),    // 大きさが上限を超える
        with_run(L"1,0,0,1,0,0,1e39,0,16,0,1,400,0,0:0:1:a"),   // 位置が float に収まらない
        with_run(L"1,0,0,1,0,0,0,0,16,0,5,400,0,0:0:1:a"),      // 不透明度が 1 を超える
        with_run(L"1,0,0,1,0,0,0,0,16,0,1,0,0,0:0:1:a"),        // 太さが 0
        with_run(L"1,0,0,1,0,0,0,0,16,0,1,400,2,0:0:1:a"),      // 斜体が 0/1 でない
        with_run(L"1,0,0,1,0,0,0,0,16,16777216,1,400,0,0:0:1:a"), // 色が 24 ビットを超える
        with_run(L"1,0,0,1,0,0,0,0,16,0,1,400,0,0:0:2:a"),      // 文字の長さが足りない
        with_run(L"1,0,0,1,0,0,0,0,16,0,1,400,0,0:-1:1:a"),     // 長さが数字でない
    };
    for (const auto& payload : kBad) {
        SCOPED_TRACE(::testing::PrintToString(payload));
        EXPECT_FALSE(mermaid_util::ParseDrawing(payload).has_value());
    }
}

// ============================================================
// SanitizeErrorMessage テスト
// ============================================================

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

// ============================================================
// NodeDiagramHash テスト
// ============================================================

namespace {

Node ParseSingleCodeBlock(std::string_view md)
{
    auto nodes = ParseNodes(md);
    EXPECT_EQ(nodes.size(), 1u);
    return std::move(nodes.front());
}

} // namespace

TEST(NodeDiagramHash, DependsOnTextWidthBucketAndTheme)
{
    const auto a = ParseSingleCodeBlock("```mermaid\ngraph TD; A-->B\n```");
    const auto b = ParseSingleCodeBlock("```mermaid\ngraph TD; A-->C\n```");
    const uint64_t base = mermaid_util::NodeDiagramHash(a, 600.0f, false);

    EXPECT_NE(mermaid_util::NodeDiagramHash(b, 600.0f, false), base);
    EXPECT_NE(mermaid_util::NodeDiagramHash(a, 600.0f, true), base);
    EXPECT_NE(mermaid_util::NodeDiagramHash(a, 900.0f, false), base);
}

// 幅は 32 刻みで量子化されるため、同じバケット内のリサイズではキャッシュを再利用する。
TEST(NodeDiagramHash, SameWidthBucketSharesHash)
{
    const auto a = ParseSingleCodeBlock("```mermaid\ngraph TD; A-->B\n```");
    ASSERT_EQ(mermaid_util::QuantizeWidth(590.0f), mermaid_util::QuantizeWidth(605.0f));
    EXPECT_EQ(mermaid_util::NodeDiagramHash(a, 590.0f, false), mermaid_util::NodeDiagramHash(a, 605.0f, false));
}

// ディスクキャッシュのファイル名になるので式を変えない。変えると既存のキャッシュを引けなくなる。
TEST(NodeDiagramHash, KeepsPersistedFormula)
{
    const auto a = ParseSingleCodeBlock("```mermaid\ngraph TD; A-->B\n```");
    // 幅 600 は 608 に量子化される。
    const uint64_t expected = Fnv1a64(a.GetText()) ^ (uint64_t{ 608 } * kFnv1a64Prime) ^ 2654435761ULL;
    EXPECT_EQ(mermaid_util::NodeDiagramHash(a, 600.0f, true), expected);
    EXPECT_EQ(mermaid_util::DiagramRequestHash(mermaid_util::DiagramCodeHash(a), 600.0f, true), expected);
    EXPECT_EQ(mermaid_util::NodeDiagramHash(a, 600.0f, false), Fnv1a64(a.GetText()) ^ (uint64_t{ 608 } * kFnv1a64Prime));
}

// 幅を混ぜないキーは、どの幅のキーとも重ならず、テーマでは分かれる。ディスクキャッシュのファイル名になるので式を変えない。
TEST(DiagramWidthFreeHash, DistinctFromWidthKeysAndKeepsFormula)
{
    const auto a = ParseSingleCodeBlock("```mermaid\ngraph TD; A-->B\n```");
    const uint64_t code = mermaid_util::DiagramCodeHash(a);
    const uint64_t free_light = mermaid_util::DiagramWidthFreeHash(code, false);
    EXPECT_EQ(free_light, Fnv1a64(a.GetText()) ^ kFnv1a64Prime);
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
