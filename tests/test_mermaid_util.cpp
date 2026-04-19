#include <gtest/gtest.h>
#include "mermaid_util.h"

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

TEST(QuantizeWidth, RoundsUpToNearest100)
{
    EXPECT_EQ(mermaid_util::QuantizeWidth(750.0f), 800);
    EXPECT_EQ(mermaid_util::QuantizeWidth(801.0f), 900);
    EXPECT_EQ(mermaid_util::QuantizeWidth(1.0f), 100);
    EXPECT_EQ(mermaid_util::QuantizeWidth(99.0f), 100);
    EXPECT_EQ(mermaid_util::QuantizeWidth(1920.0f), 2000);
}

TEST(QuantizeWidth, ExactMultiplesUnchanged)
{
    EXPECT_EQ(mermaid_util::QuantizeWidth(100.0f), 100);
    EXPECT_EQ(mermaid_util::QuantizeWidth(200.0f), 200);
    EXPECT_EQ(mermaid_util::QuantizeWidth(800.0f), 800);
    EXPECT_EQ(mermaid_util::QuantizeWidth(1000.0f), 1000);
}

TEST(QuantizeWidth, ZeroAndNegativeReturn100)
{
    EXPECT_EQ(mermaid_util::QuantizeWidth(0.0f), 100);
    EXPECT_EQ(mermaid_util::QuantizeWidth(-1.0f), 100);
    EXPECT_EQ(mermaid_util::QuantizeWidth(-100.0f), 100);
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
