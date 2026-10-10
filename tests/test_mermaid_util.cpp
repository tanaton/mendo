#include <gtest/gtest.h>
#include "mermaid_util.h"
#include "parser.h"

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

// ============================================================
// CombinedHash テスト
// ============================================================

TEST(CombinedHash, SameInputProducesSameHash)
{
    auto h1 = mermaid_util::CombinedHash("graph TD; A-->B;", 800, false);
    auto h2 = mermaid_util::CombinedHash("graph TD; A-->B;", 800, false);
    EXPECT_EQ(h1, h2);
}

TEST(CombinedHash, DifferentCodeProducesDifferentHash)
{
    auto h1 = mermaid_util::CombinedHash("graph TD; A-->B;", 800, false);
    auto h2 = mermaid_util::CombinedHash("graph LR; A-->B;", 800, false);
    EXPECT_NE(h1, h2);
}

TEST(CombinedHash, DifferentWidthProducesDifferentHash)
{
    auto h1 = mermaid_util::CombinedHash("graph TD; A-->B;", 800, false);
    auto h2 = mermaid_util::CombinedHash("graph TD; A-->B;", 600, false);
    EXPECT_NE(h1, h2);
}

TEST(CombinedHash, DifferentDarkModeProducesDifferentHash)
{
    auto h1 = mermaid_util::CombinedHash("graph TD; A-->B;", 800, false);
    auto h2 = mermaid_util::CombinedHash("graph TD; A-->B;", 800, true);
    EXPECT_NE(h1, h2);
}

TEST(CombinedHash, IdenticalDiagramsShareCacheKey)
{
    // 同じ図が複数配置されている場合、同一ハッシュでキャッシュを共有する
    std::string_view diagram = "sequenceDiagram\n    Alice->>Bob: Hello\n    Bob-->>Alice: Hi";
    auto h1 = mermaid_util::CombinedHash(diagram, 1000, false);
    auto h2 = mermaid_util::CombinedHash(diagram, 1000, false);
    auto h3 = mermaid_util::CombinedHash(diagram, 1000, false);
    EXPECT_EQ(h1, h2);
    EXPECT_EQ(h2, h3);
}

TEST(CombinedHash, WidthZero)
{
    auto h1 = mermaid_util::CombinedHash("graph TD;", 0, false);
    auto h2 = mermaid_util::CombinedHash("graph TD;", 1, false);
    EXPECT_NE(h1, h2);
}

TEST(CombinedHash, EmptyCode)
{
    auto h1 = mermaid_util::CombinedHash("", 800, false);
    auto h2 = mermaid_util::CombinedHash("", 800, true);
    EXPECT_NE(h1, h2);
}

// ============================================================
// ComputeWorkerCount テスト
// ============================================================

TEST(ComputeWorkerCount, HalfOfProcessorsClampedToTwoThroughFour)
{
    struct Case {
        unsigned int processors;
        int expected;
    };
    constexpr Case kCases[] = {
        { 0, 2 },
        { 1, 2 },
        { 2, 2 },
        { 3, 2 },
        { 4, 2 },
        { 6, 3 },
        { 8, 4 },
        { 12, 4 },
        { 16, 4 },
        { 128, 4 },
    };
    for (const auto& c : kCases) {
        SCOPED_TRACE(c.processors);
        EXPECT_EQ(mermaid_util::ComputeWorkerCount(c.processors), c.expected);
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

// ═══════════════════════════════════════════════
// BuildLatexFlowchartCode
// ═══════════════════════════════════════════════

TEST(BuildLatexFlowchartCode, WrapsInFlowchartNode)
{
    auto code = mermaid_util::BuildLatexFlowchartCode(L"E=mc^2");
    // 基本構造: flowchart LR ヘッダ + $$...$$ ラベル + style 透明化
    EXPECT_NE(code.find(L"flowchart LR"), std::wstring::npos);
    EXPECT_NE(code.find(L"$$E=mc^2$$"), std::wstring::npos);
    EXPECT_NE(code.find(L"style A fill:none,stroke:none"), std::wstring::npos);
}

TEST(BuildLatexFlowchartCode, EscapesDoubleQuote)
{
    // LaTeX 内の " は mermaid ラベルを閉じてしまうため #quot; に置換される
    auto code = mermaid_util::BuildLatexFlowchartCode(L"a\"b");
    EXPECT_NE(code.find(L"a#quot;b"), std::wstring::npos);
    EXPECT_EQ(code.find(L"a\"b"), std::wstring::npos);
}

TEST(BuildLatexFlowchartCode, EscapesClosingBracket)
{
    // ] も mermaid ラベル終端を避けるため #93; に置換される
    auto code = mermaid_util::BuildLatexFlowchartCode(L"a]b");
    EXPECT_NE(code.find(L"a#93;b"), std::wstring::npos);
}

TEST(BuildLatexFlowchartCode, ReplacesNewlinesWithSpace)
{
    auto code = mermaid_util::BuildLatexFlowchartCode(L"a\nb\rc");
    // 改行がラベル内に残っていないこと（空白に置換）
    EXPECT_NE(code.find(L"$$a b c$$"), std::wstring::npos);
}

TEST(BuildLatexFlowchartCode, PreservesBackslashForKaTeX)
{
    // LaTeX コマンドのバックスラッシュは KaTeX に渡すためそのまま保持する
    auto code = mermaid_util::BuildLatexFlowchartCode(L"\\frac{a}{b}");
    EXPECT_NE(code.find(L"$$\\frac{a}{b}$$"), std::wstring::npos);
}

// ============================================================
// ParseJson* テスト
// ============================================================

TEST(ParseJsonNumber, ParsesValueAfterKey)
{
    struct Case {
        std::wstring_view json;
        std::wstring_view key;
        float expected;
    };
    constexpr auto kMulti = L"{\"width\":100,\"height\":200,\"dpr\":2}"sv;
    constexpr Case kCases[] = {
        { L"{\"ok\":true}"sv, L"\"width\""sv, 0.0f },
        { L""sv, L"\"width\""sv, 0.0f },
        { L"{\"width\":400}"sv, L"\"width\""sv, 400.0f },
        { L"{\"width\": 400}"sv, L"\"width\""sv, 400.0f },
        { L"{\"dpr\":1.5}"sv, L"\"dpr\""sv, 1.5f },
        { kMulti, L"\"width\""sv, 100.0f },
        { kMulti, L"\"height\""sv, 200.0f },
        { kMulti, L"\"dpr\""sv, 2.0f },
        // key だけで値が無ければ 0
        { L"\"width\":"sv, L"\"width\""sv, 0.0f },
    };
    for (const auto& c : kCases) {
        SCOPED_TRACE(::testing::PrintToString(std::wstring{ c.json }) + " key=" + ::testing::PrintToString(std::wstring{ c.key }));
        EXPECT_EQ(mermaid_util::ParseJsonNumber(c.json, c.key), c.expected);
    }
}

TEST(ParseJsonTrueFlag, TrueOnlyForExplicitTrue)
{
    struct Case {
        std::wstring_view json;
        bool expected;
    };
    constexpr Case kCases[] = {
        { L"{\"width\":100}"sv, false },
        { L"{\"ok\":true}"sv, true },
        { L"{\"ok\": true}"sv, true },
        { L"{\"ok\":false}"sv, false },
        { L""sv, false },
        // キー直後にコロンが来ない異常形式
        { L"\"ok\" true"sv, false },
    };
    for (const auto& c : kCases) {
        SCOPED_TRACE(::testing::PrintToString(std::wstring{ c.json }));
        EXPECT_EQ(mermaid_util::ParseJsonTrueFlag(c.json, L"\"ok\""), c.expected);
    }
}

TEST(ParseJsonString, ExtractsAndUnescapes)
{
    struct Case {
        std::wstring_view json;
        std::wstring_view expected;
    };
    constexpr Case kCases[] = {
        { L"{\"ok\":false}"sv, L""sv },
        { L""sv, L""sv },
        { L"{\"ok\":false,\"error\":\"boom\"}"sv, L"boom"sv },
        { L"{\"error\": \"boom\"}"sv, L"boom"sv },
        { L"{\"error\":123}"sv, L""sv },
        { L"{\"error\":\"boom"sv, L""sv },
        { L"{\"error\":\"a\\nb\\t\\\"c\\\"\\\\d\"}"sv, L"a\nb\t\"c\"\\d"sv },
        { L"{\"error\":\"\\u0041\\u3042\"}"sv, L"Aあ"sv },
        { L"{\"error\":\"\\uZZZZ\"}"sv, L""sv },
        // JSON.stringify が生成する実際の mermaid エラー形式
        { L"{\"ok\":false,\"error\":\"Parse error on line 2:\\n...graph TD\\n----^\\nExpecting 'SEMI'\"}"sv,
          L"Parse error on line 2:\n...graph TD\n----^\nExpecting 'SEMI'"sv },
    };
    for (const auto& c : kCases) {
        SCOPED_TRACE(::testing::PrintToString(std::wstring{ c.json }));
        EXPECT_EQ(mermaid_util::ParseJsonString(c.json, L"\"error\""), c.expected);
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
        // JSON の \u00XX から復元された制御文字も豆腐表示にせず空白に潰す
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
        { L"render-result:12:{\"ok\":true}"sv, Kind::RenderResult, true, 12, L"{\"ok\":true}"sv },
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
    auto nodes = ParseMarkdown(md).nodes;
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
    math.ensure_code()->code_language = SyntaxLanguage::LatexMath;

    EXPECT_NE(mermaid_util::NodeDiagramHash(mermaid, 600.0f, false), mermaid_util::NodeDiagramHash(math, 600.0f, false));
}
