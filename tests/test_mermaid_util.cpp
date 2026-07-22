#include <gtest/gtest.h>
#include "mermaid_util.h"

using namespace std::literals;

// ============================================================
// JsEscape テスト
// ============================================================

TEST(JsEscape, EmptyString)
{
    EXPECT_EQ(mermaid_util::JsEscape(L""), L"");
}

TEST(JsEscape, PlainText)
{
    EXPECT_EQ(mermaid_util::JsEscape(L"hello world"), L"hello world");
}

TEST(JsEscape, BackslashEscaped)
{
    EXPECT_EQ(mermaid_util::JsEscape(L"a\\b"), L"a\\\\b");
}

TEST(JsEscape, SingleQuoteEscaped)
{
    EXPECT_EQ(mermaid_util::JsEscape(L"it's"), L"it\\'s");
}

TEST(JsEscape, DoubleQuoteEscaped)
{
    EXPECT_EQ(mermaid_util::JsEscape(L"a\"b"), L"a\\\"b");
}

TEST(JsEscape, NewlineEscaped)
{
    EXPECT_EQ(mermaid_util::JsEscape(L"a\nb"), L"a\\nb");
}

TEST(JsEscape, CarriageReturnEscaped)
{
    EXPECT_EQ(mermaid_util::JsEscape(L"a\rb"), L"a\\rb");
}

TEST(JsEscape, TabEscaped)
{
    EXPECT_EQ(mermaid_util::JsEscape(L"a\tb"), L"a\\tb");
}

TEST(JsEscape, BacktickEscaped)
{
    EXPECT_EQ(mermaid_util::JsEscape(L"a`b"), L"a\\`b");
}

TEST(JsEscape, DollarEscaped)
{
    EXPECT_EQ(mermaid_util::JsEscape(L"a$b"), L"a\\$b");
}

TEST(JsEscape, ControlCharEscaped)
{
    // ベル文字 (0x07)
    std::wstring input(1, L'\x07');
    EXPECT_EQ(mermaid_util::JsEscape(input), L"\\u0007");
}

TEST(JsEscape, MermaidDiagramCode)
{
    std::wstring code = L"graph TD;\n  A-->B;\n  B-->C;";
    auto escaped = mermaid_util::JsEscape(code);
    // 生の改行を含まないこと
    EXPECT_EQ(escaped.find(L'\n'), std::wstring::npos);
    // エスケープされた改行を含むこと
    EXPECT_NE(escaped.find(L"\\n"), std::wstring::npos);
}

TEST(JsEscape, AllSpecialChars)
{
    auto result = mermaid_util::JsEscape(L"\\\'\"\n\r\t`$");
    EXPECT_EQ(result, L"\\\\\\'\\\"\\n\\r\\t\\`\\$");
}

TEST(JsEscape, LineSeparatorEscaped)
{
    std::wstring input(1, L'\x2028');
    EXPECT_EQ(mermaid_util::JsEscape(input), L"\\u2028");
}

TEST(JsEscape, ParagraphSeparatorEscaped)
{
    std::wstring input(1, L'\x2029');
    EXPECT_EQ(mermaid_util::JsEscape(input), L"\\u2029");
}

// ============================================================
// SimpleHash テスト
// ============================================================

TEST(SimpleHash, EmptyString)
{
    auto hash = mermaid_util::SimpleHash(L"");
    EXPECT_EQ(hash.size(), 16u); // 16進数文字16文字
}

TEST(SimpleHash, DeterministicOutput)
{
    auto h1 = mermaid_util::SimpleHash(L"hello");
    auto h2 = mermaid_util::SimpleHash(L"hello");
    EXPECT_EQ(h1, h2);
}

TEST(SimpleHash, DifferentInputsDifferentHashes)
{
    auto h1 = mermaid_util::SimpleHash(L"hello");
    auto h2 = mermaid_util::SimpleHash(L"world");
    EXPECT_NE(h1, h2);
}

TEST(SimpleHash, HashLength)
{
    auto hash = mermaid_util::SimpleHash(L"test input");
    EXPECT_EQ(hash.size(), 16u);
    // 有効な16進数文字であること
    for (wchar_t c : hash) {
        EXPECT_TRUE((c >= L'0' && c <= L'9') || (c >= L'a' && c <= L'f'));
    }
}

TEST(SimpleHash, SingleCharDifference)
{
    auto h1 = mermaid_util::SimpleHash(L"abc");
    auto h2 = mermaid_util::SimpleHash(L"abd");
    EXPECT_NE(h1, h2);
}

TEST(SimpleHash, LongInput)
{
    std::wstring input(10000, L'a');
    auto hash = mermaid_util::SimpleHash(input);
    EXPECT_EQ(hash.size(), 16u);
}

// ============================================================
// CombinedHash テスト
// ============================================================

TEST(CombinedHash, SameInputProducesSameHash)
{
    auto h1 = mermaid_util::CombinedHash(L"graph TD; A-->B;", 800, false);
    auto h2 = mermaid_util::CombinedHash(L"graph TD; A-->B;", 800, false);
    EXPECT_EQ(h1, h2);
}

TEST(CombinedHash, DifferentCodeProducesDifferentHash)
{
    auto h1 = mermaid_util::CombinedHash(L"graph TD; A-->B;", 800, false);
    auto h2 = mermaid_util::CombinedHash(L"graph LR; A-->B;", 800, false);
    EXPECT_NE(h1, h2);
}

TEST(CombinedHash, DifferentWidthProducesDifferentHash)
{
    auto h1 = mermaid_util::CombinedHash(L"graph TD; A-->B;", 800, false);
    auto h2 = mermaid_util::CombinedHash(L"graph TD; A-->B;", 600, false);
    EXPECT_NE(h1, h2);
}

TEST(CombinedHash, DifferentDarkModeProducesDifferentHash)
{
    auto h1 = mermaid_util::CombinedHash(L"graph TD; A-->B;", 800, false);
    auto h2 = mermaid_util::CombinedHash(L"graph TD; A-->B;", 800, true);
    EXPECT_NE(h1, h2);
}

TEST(CombinedHash, IdenticalDiagramsShareCacheKey)
{
    // 同じ図が複数配置されている場合、同一ハッシュでキャッシュを共有する
    std::wstring_view diagram = L"sequenceDiagram\n    Alice->>Bob: Hello\n    Bob-->>Alice: Hi";
    auto h1 = mermaid_util::CombinedHash(diagram, 1000, false);
    auto h2 = mermaid_util::CombinedHash(diagram, 1000, false);
    auto h3 = mermaid_util::CombinedHash(diagram, 1000, false);
    EXPECT_EQ(h1, h2);
    EXPECT_EQ(h2, h3);
}

TEST(CombinedHash, WidthZero)
{
    auto h1 = mermaid_util::CombinedHash(L"graph TD;", 0, false);
    auto h2 = mermaid_util::CombinedHash(L"graph TD;", 1, false);
    EXPECT_NE(h1, h2);
}

TEST(CombinedHash, EmptyCode)
{
    auto h1 = mermaid_util::CombinedHash(L"", 800, false);
    auto h2 = mermaid_util::CombinedHash(L"", 800, true);
    EXPECT_NE(h1, h2);
}

// ============================================================
// ComputeWorkerCount テスト
// ============================================================

TEST(ComputeWorkerCount, ZeroProcessorsReturnsMinimum)
{
    EXPECT_EQ(mermaid_util::ComputeWorkerCount(0), 2);
}

TEST(ComputeWorkerCount, OneProcessorReturnsMinimum)
{
    EXPECT_EQ(mermaid_util::ComputeWorkerCount(1), 2);
}

TEST(ComputeWorkerCount, TwoProcessorsReturnsMinimum)
{
    // 2 / 2 = 1 → クランプで2
    EXPECT_EQ(mermaid_util::ComputeWorkerCount(2), 2);
}

TEST(ComputeWorkerCount, ThreeProcessorsReturnsMinimum)
{
    // 3 / 2 = 1 → クランプで2
    EXPECT_EQ(mermaid_util::ComputeWorkerCount(3), 2);
}

TEST(ComputeWorkerCount, FourProcessorsReturnsTwo)
{
    // 4 / 2 = 2
    EXPECT_EQ(mermaid_util::ComputeWorkerCount(4), 2);
}

TEST(ComputeWorkerCount, SixProcessorsReturnsThree)
{
    // 6 / 2 = 3
    EXPECT_EQ(mermaid_util::ComputeWorkerCount(6), 3);
}

TEST(ComputeWorkerCount, EightProcessorsReturnsFour)
{
    // 8 / 2 = 4
    EXPECT_EQ(mermaid_util::ComputeWorkerCount(8), 4);
}

TEST(ComputeWorkerCount, TwelveProcessorsReturnsMaximum)
{
    // 12 / 2 = 6 → クランプで4
    EXPECT_EQ(mermaid_util::ComputeWorkerCount(12), 4);
}

TEST(ComputeWorkerCount, SixteenProcessorsReturnsMaximum)
{
    // 16 / 2 = 8 → クランプで4
    EXPECT_EQ(mermaid_util::ComputeWorkerCount(16), 4);
}

TEST(ComputeWorkerCount, LargeProcessorCountReturnsMaximum)
{
    EXPECT_EQ(mermaid_util::ComputeWorkerCount(128), 4);
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
// ParseJsonNumber テスト
// ============================================================

TEST(ParseJsonNumber, MissingKeyReturnsZero)
{
    EXPECT_EQ(mermaid_util::ParseJsonNumber(L"{\"ok\":true}", L"\"width\""), 0.0f);
}

TEST(ParseJsonNumber, EmptyJsonReturnsZero)
{
    EXPECT_EQ(mermaid_util::ParseJsonNumber(L"", L"\"width\""), 0.0f);
}

TEST(ParseJsonNumber, BasicInteger)
{
    EXPECT_EQ(mermaid_util::ParseJsonNumber(L"{\"width\":400}", L"\"width\""), 400.0f);
}

TEST(ParseJsonNumber, AllowsSpaceAfterColon)
{
    EXPECT_EQ(mermaid_util::ParseJsonNumber(L"{\"width\": 400}", L"\"width\""), 400.0f);
}

TEST(ParseJsonNumber, FloatingPoint)
{
    EXPECT_FLOAT_EQ(mermaid_util::ParseJsonNumber(L"{\"dpr\":1.5}", L"\"dpr\""), 1.5f);
}

TEST(ParseJsonNumber, ParsesMultipleKeys)
{
    std::wstring_view json = L"{\"width\":100,\"height\":200,\"dpr\":2}";
    EXPECT_EQ(mermaid_util::ParseJsonNumber(json, L"\"width\""), 100.0f);
    EXPECT_EQ(mermaid_util::ParseJsonNumber(json, L"\"height\""), 200.0f);
    EXPECT_EQ(mermaid_util::ParseJsonNumber(json, L"\"dpr\""), 2.0f);
}

TEST(ParseJsonNumber, KeyAtEndWithoutValueReturnsZero)
{
    // 末尾直前に key だけが現れた場合は 0 を返す
    EXPECT_EQ(mermaid_util::ParseJsonNumber(L"\"width\":", L"\"width\""), 0.0f);
}

// ============================================================
// ParseJsonTrueFlag テスト
// ============================================================

TEST(ParseJsonTrueFlag, MissingKeyReturnsFalse)
{
    EXPECT_FALSE(mermaid_util::ParseJsonTrueFlag(L"{\"width\":100}", L"\"ok\""));
}

TEST(ParseJsonTrueFlag, ExplicitTrue)
{
    EXPECT_TRUE(mermaid_util::ParseJsonTrueFlag(L"{\"ok\":true}", L"\"ok\""));
}

TEST(ParseJsonTrueFlag, AllowsSpaceAfterColon)
{
    EXPECT_TRUE(mermaid_util::ParseJsonTrueFlag(L"{\"ok\": true}", L"\"ok\""));
}

TEST(ParseJsonTrueFlag, ExplicitFalseReturnsFalse)
{
    EXPECT_FALSE(mermaid_util::ParseJsonTrueFlag(L"{\"ok\":false}", L"\"ok\""));
}

TEST(ParseJsonTrueFlag, EmptyJsonReturnsFalse)
{
    EXPECT_FALSE(mermaid_util::ParseJsonTrueFlag(L"", L"\"ok\""));
}

TEST(ParseJsonTrueFlag, KeyWithoutColonReturnsFalse)
{
    // キー直後にコロンが来ない異常形式は false
    EXPECT_FALSE(mermaid_util::ParseJsonTrueFlag(L"\"ok\" true", L"\"ok\""));
}

// ============================================================
// ParseJsonString テスト
// ============================================================

TEST(ParseJsonString, MissingKeyReturnsEmpty)
{
    EXPECT_TRUE(mermaid_util::ParseJsonString(L"{\"ok\":false}", L"\"error\"").empty());
}

TEST(ParseJsonString, EmptyJsonReturnsEmpty)
{
    EXPECT_TRUE(mermaid_util::ParseJsonString(L"", L"\"error\"").empty());
}

TEST(ParseJsonString, BasicString)
{
    EXPECT_EQ(mermaid_util::ParseJsonString(L"{\"ok\":false,\"error\":\"boom\"}", L"\"error\""), L"boom");
}

TEST(ParseJsonString, AllowsSpaceAfterColon)
{
    EXPECT_EQ(mermaid_util::ParseJsonString(L"{\"error\": \"boom\"}", L"\"error\""), L"boom");
}

TEST(ParseJsonString, NonStringValueReturnsEmpty)
{
    EXPECT_TRUE(mermaid_util::ParseJsonString(L"{\"error\":123}", L"\"error\"").empty());
}

TEST(ParseJsonString, UnterminatedStringReturnsEmpty)
{
    EXPECT_TRUE(mermaid_util::ParseJsonString(L"{\"error\":\"boom", L"\"error\"").empty());
}

TEST(ParseJsonString, UnescapesCommonEscapes)
{
    EXPECT_EQ(mermaid_util::ParseJsonString(L"{\"error\":\"a\\nb\\t\\\"c\\\"\\\\d\"}", L"\"error\""),
              L"a\nb\t\"c\"\\d");
}

TEST(ParseJsonString, UnescapesUnicodeEscape)
{
    EXPECT_EQ(mermaid_util::ParseJsonString(L"{\"error\":\"\\u0041\\u3042\"}", L"\"error\""), L"Aあ");
}

TEST(ParseJsonString, InvalidUnicodeEscapeReturnsEmpty)
{
    EXPECT_TRUE(mermaid_util::ParseJsonString(L"{\"error\":\"\\uZZZZ\"}", L"\"error\"").empty());
}

TEST(ParseJsonString, MermaidParseErrorMessage)
{
    // JSON.stringify が生成する実際のエラー形式 (改行は \n にエスケープされる)
    const auto json = L"{\"ok\":false,\"error\":\"Parse error on line 2:\\n...graph TD\\n----^\\nExpecting 'SEMI'\"}"sv;
    EXPECT_EQ(mermaid_util::ParseJsonString(json, L"\"error\""),
              L"Parse error on line 2:\n...graph TD\n----^\nExpecting 'SEMI'");
}

// ============================================================
// SanitizeErrorMessage テスト
// ============================================================

TEST(SanitizeErrorMessage, EmptyReturnsEmpty)
{
    EXPECT_TRUE(mermaid_util::SanitizeErrorMessage(L"", 100).empty());
}

TEST(SanitizeErrorMessage, PlainTextUnchanged)
{
    EXPECT_EQ(mermaid_util::SanitizeErrorMessage(L"simple error", 100), L"simple error");
}

TEST(SanitizeErrorMessage, CollapsesWhitespaceRuns)
{
    EXPECT_EQ(mermaid_util::SanitizeErrorMessage(L"Parse error on line 2:\n\n----^\t got 'X'", 100),
              L"Parse error on line 2: ----^ got 'X'");
}

TEST(SanitizeErrorMessage, CollapsesControlChars)
{
    // JSON の \u00XX から復元された制御文字 (垂直タブ・ESC 等) も豆腐表示にせず空白に潰す
    EXPECT_EQ(mermaid_util::SanitizeErrorMessage(L"got\x0b\x1bhere", 100), L"got here");
}

TEST(SanitizeErrorMessage, TrimsLeadingAndTrailingWhitespace)
{
    EXPECT_EQ(mermaid_util::SanitizeErrorMessage(L"  \n boom \n ", 100), L"boom");
}

TEST(SanitizeErrorMessage, TruncatesWithEllipsis)
{
    const auto result = mermaid_util::SanitizeErrorMessage(L"abcdefghij", 5);
    EXPECT_EQ(result, L"abcd…");
}

TEST(SanitizeErrorMessage, ExactMaxLenNotTruncated)
{
    EXPECT_EQ(mermaid_util::SanitizeErrorMessage(L"abcde", 5), L"abcde");
}

TEST(SanitizeErrorMessage, WhitespaceOnlyReturnsEmpty)
{
    EXPECT_TRUE(mermaid_util::SanitizeErrorMessage(L" \n\t ", 100).empty());
}

// ============================================================
// ParseRequestPrefix テスト
// ============================================================

TEST(ParseRequestPrefix, EmptyReturnsInvalid)
{
    auto p = mermaid_util::ParseRequestPrefix(L"");
    EXPECT_FALSE(p.valid);
}

TEST(ParseRequestPrefix, NonDigitPrefixReturnsInvalid)
{
    auto p = mermaid_util::ParseRequestPrefix(L"abc:123");
    EXPECT_FALSE(p.valid);
}

TEST(ParseRequestPrefix, IdOnlyWithoutPayload)
{
    auto p = mermaid_util::ParseRequestPrefix(L"42");
    EXPECT_TRUE(p.valid);
    EXPECT_EQ(p.id, 42u);
    EXPECT_FALSE(p.has_payload);
    EXPECT_TRUE(p.payload.empty());
}

TEST(ParseRequestPrefix, IdWithEmptyPayload)
{
    // ID の直後に ':' はあるが payload が空のケース
    auto p = mermaid_util::ParseRequestPrefix(L"7:");
    EXPECT_TRUE(p.valid);
    EXPECT_EQ(p.id, 7u);
    EXPECT_TRUE(p.has_payload);
    EXPECT_TRUE(p.payload.empty());
}

TEST(ParseRequestPrefix, IdWithJsonPayload)
{
    auto p = mermaid_util::ParseRequestPrefix(L"123:{\"ok\":true}");
    EXPECT_TRUE(p.valid);
    EXPECT_EQ(p.id, 123u);
    EXPECT_TRUE(p.has_payload);
    EXPECT_EQ(p.payload, L"{\"ok\":true}");
}

TEST(ParseRequestPrefix, LargeId)
{
    auto p = mermaid_util::ParseRequestPrefix(L"4294967290:done");
    EXPECT_TRUE(p.valid);
    EXPECT_EQ(p.id, 4294967290u);
    EXPECT_TRUE(p.has_payload);
    EXPECT_EQ(p.payload, L"done");
}

TEST(ParseRequestPrefix, TrailingGarbageWithoutColon)
{
    // 数字の直後がコロンでなければ payload は付かない
    auto p = mermaid_util::ParseRequestPrefix(L"42abc");
    EXPECT_TRUE(p.valid);
    EXPECT_EQ(p.id, 42u);
    EXPECT_FALSE(p.has_payload);
}

TEST(ParseRequestPrefix, OverflowingIdReturnsInvalid)
{
    // UINT_MAX (4294967295) を超える ID は無効として扱う
    auto p = mermaid_util::ParseRequestPrefix(L"4294967296:x");
    EXPECT_FALSE(p.valid);
}

TEST(ParseRequestPrefix, VeryLongDigitsOverflowStillInvalid)
{
    // 旧実装の 16 文字固定バッファでは先頭だけが残って誤った ID になっていたケース。
    // 現在は uint64 アキュムレータで溢れを検知して無効化する。
    auto p = mermaid_util::ParseRequestPrefix(L"99999999999999999999:payload");
    EXPECT_FALSE(p.valid);
}

TEST(ParseRequestPrefix, MaxUintBoundary)
{
    // UINT_MAX ちょうどは有効
    auto p = mermaid_util::ParseRequestPrefix(L"4294967295:ok");
    EXPECT_TRUE(p.valid);
    EXPECT_EQ(p.id, 4294967295u);
    EXPECT_TRUE(p.has_payload);
    EXPECT_EQ(p.payload, L"ok");
}
