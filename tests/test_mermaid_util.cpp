#include <gtest/gtest.h>
#include "mermaid_util.h"
#include "parser.h"
#include "test_nodes.h"
#include "utility.h"

using namespace std::literals;

// ============================================================
// JsEscape テスト
// ============================================================

TEST(JsEscape, EscapesJsStringLiteralSpecials)
{
    struct Case {
        std::wstring_view input;
        std::wstring_view expected;
    };
    constexpr Case kCases[] = {
        { L""sv, L""sv },
        { L"hello world"sv, L"hello world"sv },
        { L"a\\b"sv, L"a\\\\b"sv },
        { L"it's"sv, L"it\\'s"sv },
        { L"a\"b"sv, L"a\\\"b"sv },
        { L"a\nb"sv, L"a\\nb"sv },
        { L"a\rb"sv, L"a\\rb"sv },
        { L"a\tb"sv, L"a\\tb"sv },
        { L"a`b"sv, L"a\\`b"sv },
        { L"a$b"sv, L"a\\$b"sv },
        { L"\\\'\"\n\r\t`$"sv, L"\\\\\\'\\\"\\n\\r\\t\\`\\$"sv },
        { L"\x07"sv, L"\\u0007"sv },
        { L"\x2028"sv, L"\\u2028"sv },
        { L"\x2029"sv, L"\\u2029"sv },
        { L"graph TD;\n  A-->B;\n  B-->C;"sv, L"graph TD;\\n  A-->B;\\n  B-->C;"sv },
    };
    for (const auto& c : kCases) {
        SCOPED_TRACE(::testing::PrintToString(std::wstring{ c.input }));
        EXPECT_EQ(mermaid_util::JsEscape(c.input), c.expected);
    }
}

// ═══════════════════════════════════════════════
// QuantizeWidth
// ═══════════════════════════════════════════════

TEST(QuantizeWidth, RoundsUpToNearest32)
{
    EXPECT_EQ(mermaid_util::QuantizeWidth(750.0f), 768);
    EXPECT_EQ(mermaid_util::QuantizeWidth(801.0f), 832);
    EXPECT_EQ(mermaid_util::QuantizeWidth(1.0f), 32);
    EXPECT_EQ(mermaid_util::QuantizeWidth(31.0f), 32);
    EXPECT_EQ(mermaid_util::QuantizeWidth(1920.0f), 1920);
    EXPECT_EQ(mermaid_util::QuantizeWidth(1921.0f), 1952);
}

TEST(QuantizeWidth, ExactMultiplesUnchanged)
{
    EXPECT_EQ(mermaid_util::QuantizeWidth(32.0f), 32);
    EXPECT_EQ(mermaid_util::QuantizeWidth(64.0f), 64);
    EXPECT_EQ(mermaid_util::QuantizeWidth(800.0f), 800);
    EXPECT_EQ(mermaid_util::QuantizeWidth(1024.0f), 1024);
}

TEST(QuantizeWidth, ZeroAndNegativeReturnMinimum)
{
    EXPECT_EQ(mermaid_util::QuantizeWidth(0.0f), 32);
    EXPECT_EQ(mermaid_util::QuantizeWidth(-1.0f), 32);
    EXPECT_EQ(mermaid_util::QuantizeWidth(-100.0f), 32);
}

// ============================================================
// ParseRenderSize テスト
// ============================================================

// ============================================================
// ParseMathBatch
// ============================================================

TEST(ParseMathBatch, ParsesBoxesAndFailedItems)
{
    const auto layout = mermaid_util::ParseMathBatch(L"1024:40:1.5|2,2,30.5,20,15;e;40,2,10,24,18", 3);
    ASSERT_TRUE(layout);
    EXPECT_FLOAT_EQ(layout->size.width, 1024.0f);
    EXPECT_FLOAT_EQ(layout->size.dpr, 1.5f);
    ASSERT_EQ(layout->boxes.size(), 3u);
    EXPECT_FLOAT_EQ(layout->boxes[0].width, 30.5f);
    EXPECT_FLOAT_EQ(layout->boxes[0].baseline, 15.0f);
    EXPECT_FLOAT_EQ(layout->boxes[1].width, 0.0f);
    EXPECT_FLOAT_EQ(layout->boxes[2].x, 40.0f);
}

// 数が合わない結果や、ページの外を指す箱は使わない (型の外を切り出させない)。
TEST(ParseMathBatch, RejectsMismatchedCountAndOutOfPageBoxes)
{
    for (const auto* payload : {
             L"100:40:1|2,2,10,10,8",
             L"100:40:1|2,2,10,10,8;2,2,10,10,8;2,2,10,10,8",
             L"100:40:1|95,2,10,10,8;2,2,10,10,8",
             L"100:40:1|2,35,10,10,8;2,2,10,10,8",
             L"100:40:1|-1,2,10,10,8;2,2,10,10,8",
             L"100:40:1|2,2,10,10,30;2,2,10,10,8",
             L"0:40:1|e;e",
             L"100:40:1",
         }) {
        SCOPED_TRACE(testing::PrintToString(std::wstring{ payload }));
        EXPECT_FALSE(mermaid_util::ParseMathBatch(payload, 2));
    }
}

TEST(ParseWebMessage, RecognizesMathResult)
{
    const auto parsed = mermaid_util::ParseWebMessage(L"math-result:7:100:40:1|e");
    EXPECT_EQ(parsed.kind, mermaid_util::WebMessageKind::MathResult);
    EXPECT_EQ(parsed.request.id, 7u);
    EXPECT_EQ(parsed.request.payload, L"100:40:1|e");
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
        { L"120:80:2"sv, 120.0f, 80.0f, 2.0f },
        // 欠けた値は 0 (寸法は <= 0 判定で弾かれ、DPR は既定に倒れる)
        { L""sv, 0.0f, 0.0f, 0.0f },
        { L"400"sv, 400.0f, 0.0f, 0.0f },
        { L"400:300"sv, 400.0f, 300.0f, 0.0f },
        { L"400::1"sv, 400.0f, 0.0f, 1.0f },
        // ページが偽った有限でない値は 0
        { L"nan:300:1"sv, 0.0f, 300.0f, 1.0f },
        { L"inf:1e39:1"sv, 0.0f, 0.0f, 1.0f },
        // 4 つ目以降は読まない
        { L"1:2:3:4"sv, 1.0f, 2.0f, 3.0f },
    };
    for (const auto& c : kCases) {
        SCOPED_TRACE(::testing::PrintToString(std::wstring{ c.payload }));
        const auto size = mermaid_util::ParseRenderSize(c.payload);
        EXPECT_EQ(size.width, c.width);
        EXPECT_EQ(size.height, c.height);
        EXPECT_EQ(size.dpr, c.dpr);
    }
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
        { L"capture-ready:5"sv, Kind::CaptureReady, true, 5, L""sv },
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

// 同じ本文でも LaTeX 数式と Mermaid はレンダ結果が異なるのでキャッシュを共有しない。
TEST(NodeDiagramHash, LatexMathDoesNotCollideWithMermaid)
{
    const auto mermaid = ParseSingleCodeBlock("```mermaid\nx^2\n```");
    auto math = ParseSingleCodeBlock("```mermaid\nx^2\n```");
    math.code_language = SyntaxLanguage::LatexMath;

    EXPECT_NE(mermaid_util::NodeDiagramHash(mermaid, 600.0f, false), mermaid_util::NodeDiagramHash(math, 600.0f, false));
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

TEST(IsModulePath, AcceptsMjsExceptNumericIdSyntax)
{
    EXPECT_TRUE(mermaid_util::IsModulePath(L"mermaid.esm.min.mjs"sv));
    EXPECT_TRUE(mermaid_util::IsModulePath(L"chunks/mermaid.esm.min/chunk-WLHZKB66.mjs"sv));
    EXPECT_FALSE(mermaid_util::IsModulePath(L""sv));
    EXPECT_FALSE(mermaid_util::IsModulePath(L"index.html"sv));
    EXPECT_FALSE(mermaid_util::IsModulePath(L"mermaid.esm.min.mjs?v=1"sv));
    // # で始まる名前は FindResourceW が数値 ID と読み、ESM 以外の埋め込みリソースに届きうる
    EXPECT_FALSE(mermaid_util::IsModulePath(L"#304.mjs"sv));
}
