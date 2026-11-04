#include <gtest/gtest.h>
#include "mermaid_util.h"
#include "parser.h"
#include "string_convert.h"
#include "test_nodes.h"
#include "utility.h"
#include <algorithm>
#include <vector>

using namespace std::literals;

// ============================================================
// ページへの要求
// ============================================================

// 図のコードはエスケープせずに末尾へ置く。区切りの ':' や引用符・改行を含んでもそのまま届く。
TEST(PageMessage, RenderMessagePutsRawCodeLast)
{
    EXPECT_EQ(mermaid_util::PageRenderMessage(mermaid_util::PageRequest::Mermaid, 7, false, 800.0f, L"ja-jp", "graph TD;\n  A[\"a:b\"]-->B;"),
              L"mermaid:7:0:800.00:ja-jp:graph TD;\n  A[\"a:b\"]-->B;");
    EXPECT_EQ(mermaid_util::PageRenderMessage(mermaid_util::PageRequest::Svg, 8, true, 612.4f, L"zh-hk", "x"), L"svg:8:1:612.40:zh-hk:x");
    EXPECT_EQ(mermaid_util::PageRenderMessage(mermaid_util::PageRequest::View, 9, false, 1.5f, L"ko-kr", "x"), L"view:9:0:1.50:ko-kr:x");
}

// 文言は改行で区切るので、':' を含んでいても切り出せる。
TEST(PageMessage, ViewerInitSeparatesLabelsByNewline)
{
    const std::wstring_view labels[]{ L"a:b", L"c" };
    EXPECT_EQ(mermaid_util::ViewerInitMessage(true, labels), L"init:1:a:b\nc\n");
    EXPECT_EQ(mermaid_util::ViewerInitMessage(false, {}), L"init:0:");
}


// SVG は末尾に置くので、':' を含んでいてもそのまま読める。
TEST(PageMessage, ViewerSvgPutsSvgLast)
{
    EXPECT_EQ(mermaid_util::ViewerSvgMessage(false, L"<svg style=\"a:b\"/>"), L"svg:0:<svg style=\"a:b\"/>");
    EXPECT_EQ(mermaid_util::ViewerSvgMessage(true, L"x"), L"svg:1:x");
}

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

TEST(ParseRenderSize, ParsesWidthHeightAndDpr)
{
    struct Case {
        std::wstring_view payload;
        float width;
        float height;
        float dpr;
    };
    constexpr Case kCases[] = {
        { L"400:300:1.5"sv, 400.0f, 300.0f, 1.5f },
        { L"120:80:2"sv,    120.0f, 80.0f,  2.0f },
        // 欠けた寸法は 0 (<= 0 判定で弾かれる)。DPR は欠けてもありえない値でも 1
        { L""sv,            0.0f,   0.0f,   1.0f },
        { L"400"sv,         400.0f, 0.0f,   1.0f },
        { L"400:300"sv,     400.0f, 300.0f, 1.0f },
        { L"400::1"sv,      400.0f, 0.0f,   1.0f },
        { L"400:300:100"sv, 400.0f, 300.0f, 1.0f },
        // ページが偽った有限でない値は 0
        { L"nan:300:1"sv,   0.0f,   300.0f, 1.0f },
        { L"inf:1e39:1"sv,  0.0f,   0.0f,   1.0f },
        // 4 つ目以降は読まない
        { L"1:2:3:4"sv,     1.0f,   2.0f,   3.0f },
    };
    for (const auto& c : kCases) {
        SCOPED_TRACE(::testing::PrintToString(std::wstring{ c.payload }));
        const auto size = mermaid_util::ParseRenderSize(c.payload);
        EXPECT_EQ(size.width, c.width);
        EXPECT_EQ(size.height, c.height);
        EXPECT_EQ(size.dpr, c.dpr);
    }
}

// 文字の並びの本文と書体、数式の TeX は長さで切るので、区切りの ':' や ',' を含んでもそのまま読める。
TEST(ParseDrawing, ParsesShapesTextRunsAndMaths)
{
    const auto d = mermaid_util::ParseDrawing(
        L"400:300.5:1.5:2039583:2:"
        L"12:<svg>あ</svg>2:"
        L"1,0,0,1,10.5,20,3,4.25,16,700,1,0,16711680,0.5:10:Segoe UI,x5:a:b,c"
        L"0,1,-1,0,0,0,0,0,12,400,0,1,0,1:0:0:"
        L"1:1,0,0,1,0,0,7,8.5,14,255,0.75:9:x_{a:b},c"
        L"0:0:0:"sv);
    ASSERT_TRUE(d.has_value());
    EXPECT_EQ(d->size.width, 400.0f);
    EXPECT_EQ(d->size.height, 300.5f);
    EXPECT_EQ(d->size.dpr, 1.5f);
    EXPECT_EQ(d->background, 0x1F1F1Fu);
    ASSERT_EQ(d->layers.size(), 2u);
    EXPECT_EQ(d->layers[0].shapes_svg, "<svg>\xE3\x81\x82</svg>");
    EXPECT_TRUE(d->layers[1].shapes_svg.empty());
    EXPECT_TRUE(d->layers[1].texts.empty());
    const auto& texts = d->layers[0].texts;
    ASSERT_EQ(texts.size(), 2u);
    const auto& t = texts[0];
    EXPECT_EQ(t.transform, (std::array{ 1.0f, 0.0f, 0.0f, 1.0f, 10.5f, 20.0f }));
    EXPECT_EQ(t.x, 3.0f);
    EXPECT_EQ(t.y, 4.25f);
    EXPECT_EQ(t.font_size, 16.0f);
    EXPECT_EQ(t.weight, 700);
    EXPECT_TRUE(t.italic);
    EXPECT_FALSE(t.underline);
    EXPECT_EQ(t.rgb, 0xFF0000u);
    EXPECT_EQ(t.alpha, 0.5f);
    EXPECT_EQ(t.family, L"Segoe UI,x"sv);
    EXPECT_EQ(t.text, L"a:b,c"sv);
    EXPECT_TRUE(texts[1].underline);
    EXPECT_TRUE(texts[1].family.empty());
    EXPECT_TRUE(texts[1].text.empty());
    ASSERT_EQ(d->layers[0].maths.size(), 1u);
    const auto& m = d->layers[0].maths[0];
    EXPECT_EQ(m.x, 7.0f);
    EXPECT_EQ(m.y, 8.5f);
    EXPECT_EQ(m.font_size, 14.0f);
    EXPECT_EQ(m.rgb, 0xFFu);
    EXPECT_EQ(m.alpha, 0.75f);
    EXPECT_EQ(m.tex, "x_{a:b},c");
    EXPECT_EQ(m.drawing, nullptr) << "描画命令は読んだ後に組み直す";
    EXPECT_TRUE(d->layers[1].maths.empty());
}

// 数式も書いた形のまま読み戻せる (TeX の BMP 外の文字は UTF-16 で 2 単位に数える)。
TEST(ParseDrawing, SerializedMathsParseBack)
{
    mermaid_util::DiagramDrawing drawing{ .size = { 10.0f, 20.0f, 1.5f }, .background = 0xFFFFFF };
    drawing.layers.push_back({ .maths = { { .transform = { 2.0f, 0.0f, 0.0f, 2.0f, 1.0f, 2.0f },
                                            .x = 0.5f,
                                            .y = 12.25f,
                                            .font_size = 16.0f,
                                            .rgb = 0x123456,
                                            .alpha = 1.0f,
                                            .tex = "\\mathcal{A}\xF0\x9D\x91\xA5" } } });
    const auto back = mermaid_util::ParseDrawing(string_convert::Utf8ToWide(mermaid_util::SerializeDrawing(drawing)));
    ASSERT_TRUE(back);
    EXPECT_EQ(*back, drawing);
}

// ページは XSS で偽れるので、欠け・余り・ありえない値は全体を捨てる。
TEST(ParseDrawing, RejectsMalformedPayloads)
{
    constexpr std::wstring_view kRun = L"1,0,0,1,0,0,0,0,16,400,0,0,0,1:0:1:a"sv;
    const auto with_run = [&](std::wstring_view run) { return std::wstring{ L"10:20:1:0:1:0:1:" } + std::wstring{ run } + L"0:"; };
    const auto with_math = [&](std::wstring_view math) { return std::wstring{ L"10:20:1:0:1:0:0:1:" } + std::wstring{ math }; };
    EXPECT_TRUE(mermaid_util::ParseDrawing(with_run(kRun)).has_value());
    EXPECT_TRUE(mermaid_util::ParseDrawing(with_math(L"1,0,0,1,0,0,0,0,16,0,1:1:x")).has_value());
    const std::wstring kBad[] = {
        L"",
        L"0:20:1:0:1:0:0:0:",             // 幅が 0
        L"10:20:1:16777216:1:0:0:0:",     // 背景が 24 ビットを超える
        L"10:20:1:0:1:9:<svg>0:0:",       // SVG の長さが足りない
        L"10:20:1:0:1:0:0:0:x",           // 余り
        L"10:20:1:0:2:0:0:0:",            // 層が足りない
        L"10:20:1:0:1:0:0:",              // 数式の数が無い
        L"10:20:1:0:1:0:2:" + std::wstring{ kRun } + L"0:", // 並びが足りない
        with_math(L""),                                  // 数式が足りない
        with_math(L"1,0,0,1,0,0,0,0,16,0:1:x"),           // 数式の数が 10 個
        with_math(L"1,0,0,1,0,0,0,0,0,0,1:1:x"),          // 数式の大きさが 0
        with_math(L"1,0,0,1,0,0,0,0,16,16777216,1:1:x"),  // 数式の色が 24 ビットを超える
        with_math(L"1,0,0,1,0,0,0,0,16,0,1:2:x"),         // TeX の長さが足りない
        with_run(L"1,0,0,1,0,0,0,0,16,400,0,0,0:0:1:a"),        // 数が 13 個
        with_run(L"1,0,0,1,0,0,0,0,0,400,0,0,0,1:0:1:a"),       // 大きさが 0
        with_run(L"1,0,0,1,0,0,0,0,16,0,0,0,0,1:0:1:a"),        // 太さが 0
        with_run(L"1,0,0,1,0,0,0,0,16,400,2,0,0,1:0:1:a"),      // 斜体が 0/1 でない
        with_run(L"1,0,0,1,0,0,0,0,16,400,0,0,16777216,1:0:1:a"), // 色が 24 ビットを超える
        with_run(L"1,0,0,1,0,0,0,0,16,400,0,0,0,1:0:2:a"),      // 文字の長さが足りない
        with_run(L"1,0,0,1,0,0,0,0,16,400,0,0,0,1:-1:1:a"),     // 長さが数字でない
    };
    for (const auto& payload : kBad) {
        SCOPED_TRACE(::testing::PrintToString(payload));
        EXPECT_FALSE(mermaid_util::ParseDrawing(payload).has_value());
    }
    // 不透明度は 0〜1 に収める
    const auto clamped = mermaid_util::ParseDrawing(with_run(L"1,0,0,1,0,0,0,0,16,400,0,0,0,5:0:1:a"));
    ASSERT_TRUE(clamped.has_value());
    EXPECT_EQ(clamped->layers[0].texts[0].alpha, 1.0f);
}

TEST(IsPlausibleDpr, AcceptsOnlyRealisticScales)
{
    EXPECT_TRUE(mermaid_util::IsPlausibleDpr(1.0f));
    EXPECT_TRUE(mermaid_util::IsPlausibleDpr(2.5f));
    EXPECT_FALSE(mermaid_util::IsPlausibleDpr(0.0f));
    EXPECT_FALSE(mermaid_util::IsPlausibleDpr(-1.0f));
    EXPECT_FALSE(mermaid_util::IsPlausibleDpr(100.0f));
    EXPECT_FALSE(mermaid_util::IsPlausibleDpr(std::numeric_limits<float>::quiet_NaN()));
}

TEST(FitsCaptureLimit, RejectsSidesOverBitmapMaxAndHugeAreas)
{
    EXPECT_TRUE(mermaid_util::FitsCaptureLimit(1000.0f, 10000.0f, 1.5f, 16384));
    EXPECT_TRUE(mermaid_util::FitsCaptureLimit(16384.0f, 100.0f, 1.0f, 16384));
    EXPECT_FALSE(mermaid_util::FitsCaptureLimit(11000.0f, 100.0f, 1.5f, 16384));
    EXPECT_FALSE(mermaid_util::FitsCaptureLimit(100.0f, 200000.0f, 1.0f, 16384));
    EXPECT_FALSE(mermaid_util::FitsCaptureLimit(10000.0f, 10000.0f, 1.0f, 16384));
    // 最大辺の小さい GPU ではその辺で弾く。
    EXPECT_FALSE(mermaid_util::FitsCaptureLimit(9000.0f, 100.0f, 1.0f, 8192));
}

// 面積の上限 (64Mi px) と最大辺は両端を含む。物理 px は切り上げて数える
// (切り捨てると 1px 大きいビットマップを作ろうとして失敗し、描き直しを繰り返す)。
TEST(FitsCaptureLimit, BoundariesAreInclusiveAndRoundUp)
{
    EXPECT_TRUE(mermaid_util::FitsCaptureLimit(8192.0f, 8192.0f, 1.0f, 16384));
    EXPECT_FALSE(mermaid_util::FitsCaptureLimit(8192.0f, 8193.0f, 1.0f, 16384));
    EXPECT_TRUE(mermaid_util::FitsCaptureLimit(10922.0f, 1.0f, 1.5f, 16384));  // 16383
    EXPECT_FALSE(mermaid_util::FitsCaptureLimit(10923.0f, 1.0f, 1.5f, 16384)); // 16384.5 -> 16385
    EXPECT_FALSE(mermaid_util::FitsCaptureLimit(std::numeric_limits<float>::quiet_NaN(), 100.0f, 1.0f, 16384));
    EXPECT_FALSE(mermaid_util::FitsCaptureLimit(std::numeric_limits<float>::infinity(), 100.0f, 1.0f, 16384));
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
        // ページから届いた制御文字も豆腐表示にせず空白に潰す
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
// ParseRequestPrefix テスト
// ============================================================

TEST(ParseRequestPrefix, ParsesIdAndPayload)
{
    struct Case {
        std::wstring_view body;
        bool valid;
        unsigned int id;
        bool has_payload;
        std::wstring_view payload;
    };
    constexpr Case kCases[] = {
        { L""sv, false, 0, false, L""sv },
        { L"abc:123"sv, false, 0, false, L""sv },
        { L"42"sv, true, 42, false, L""sv },
        { L"7:"sv, true, 7, true, L""sv },
        { L"123:{\"ok\":true}"sv, true, 123, true, L"{\"ok\":true}"sv },
        { L"4294967290:done"sv, true, 4294967290u, true, L"done"sv },
        // 数字の直後がコロンでなければ payload は付かない
        { L"42abc"sv, true, 42, false, L""sv },
        { L"4294967295:ok"sv, true, 4294967295u, true, L"ok"sv },
        { L"4294967296:x"sv, false, 0, false, L""sv },
        // 旧実装の 16 文字固定バッファでは先頭だけが残って誤った ID になっていた
        { L"99999999999999999999:payload"sv, false, 0, false, L""sv },
    };
    for (const auto& c : kCases) {
        SCOPED_TRACE(::testing::PrintToString(std::wstring{ c.body }));
        const auto p = mermaid_util::ParseRequestPrefix(c.body);
        EXPECT_EQ(p.valid, c.valid);
        EXPECT_EQ(p.id, c.id);
        EXPECT_EQ(p.has_payload, c.has_payload);
        EXPECT_EQ(p.payload, c.payload);
    }
}

// ============================================================
// ParseWebMessage テスト
// ============================================================

TEST(ParseWebMessage, ClassifiesMessagesAndParsesRequest)
{
    using Kind = mermaid_util::WebMessageKind;
    struct Case {
        std::wstring_view msg;
        Kind kind;
        bool request_valid;
        unsigned int id;
        std::wstring_view payload;
    };
    constexpr Case kCases[] = {
        { L"render-result:12:400:300:1.5"sv, Kind::RenderResult, true, 12, L"400:300:1.5"sv },
        { L"render-error:9:Parse error on line 2"sv, Kind::RenderError, true, 9, L"Parse error on line 2"sv },
        { L"svg-result:3:<svg/>"sv, Kind::SvgResult, true, 3, L"<svg/>"sv },
        { L"draw-result:4:10:20:1:0:0:"sv, Kind::DrawResult, true, 4, L"10:20:1:0:0:"sv },
        { L"capture-ready:5"sv, Kind::CaptureReady, true, 5, L""sv },
        // インライン数式は WebView2 で描かなくなった。
        { L"math-result:7:100:40:1|e"sv, Kind::Unknown, false, 0, L""sv },
        { L"render-error:9"sv, Kind::RenderError, true, 9, L""sv },
        { L"mermaid-failed"sv, Kind::Failed, false, 0, L""sv },
        // ID が数字でなければ kind は決まるが request は無効 (呼び出し側で破棄する)
        { L"render-result:abc"sv, Kind::RenderResult, false, 0, L""sv },
        // 完全一致でない failed や未知のメッセージは Unknown
        { L"mermaid-failed:extra"sv, Kind::Unknown, false, 0, L""sv },
        { L"render-result"sv, Kind::Unknown, false, 0, L""sv },
        { L""sv, Kind::Unknown, false, 0, L""sv },
        { L"hello"sv, Kind::Unknown, false, 0, L""sv },
    };
    for (const auto& c : kCases) {
        SCOPED_TRACE(::testing::PrintToString(std::wstring{ c.msg }));
        const auto m = mermaid_util::ParseWebMessage(c.msg);
        EXPECT_EQ(m.kind, c.kind);
        EXPECT_EQ(m.request.valid, c.request_valid);
        EXPECT_EQ(m.request.id, c.id);
        EXPECT_EQ(m.request.payload, c.payload);
    }
}

TEST(ParseWebMessage, ReadyCarriesDevicePixelRatio)
{
    const auto m = mermaid_util::ParseWebMessage(L"mermaid-ready:1.5"sv);
    EXPECT_EQ(m.kind, mermaid_util::WebMessageKind::Ready);
    EXPECT_FLOAT_EQ(m.ready_dpr, 1.5f);
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
