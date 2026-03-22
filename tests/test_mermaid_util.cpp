#include <gtest/gtest.h>
#include "mermaid_util.h"

// ============================================================
// JsEscape テスト
// ============================================================

TEST(JsEscape, EmptyString) {
    EXPECT_EQ(mermaid_util::JsEscape(L""), L"");
}

TEST(JsEscape, PlainText) {
    EXPECT_EQ(mermaid_util::JsEscape(L"hello world"), L"hello world");
}

TEST(JsEscape, BackslashEscaped) {
    EXPECT_EQ(mermaid_util::JsEscape(L"a\\b"), L"a\\\\b");
}

TEST(JsEscape, SingleQuoteEscaped) {
    EXPECT_EQ(mermaid_util::JsEscape(L"it's"), L"it\\'s");
}

TEST(JsEscape, DoubleQuoteEscaped) {
    EXPECT_EQ(mermaid_util::JsEscape(L"a\"b"), L"a\\\"b");
}

TEST(JsEscape, NewlineEscaped) {
    EXPECT_EQ(mermaid_util::JsEscape(L"a\nb"), L"a\\nb");
}

TEST(JsEscape, CarriageReturnEscaped) {
    EXPECT_EQ(mermaid_util::JsEscape(L"a\rb"), L"a\\rb");
}

TEST(JsEscape, TabEscaped) {
    EXPECT_EQ(mermaid_util::JsEscape(L"a\tb"), L"a\\tb");
}

TEST(JsEscape, BacktickEscaped) {
    EXPECT_EQ(mermaid_util::JsEscape(L"a`b"), L"a\\`b");
}

TEST(JsEscape, DollarEscaped) {
    EXPECT_EQ(mermaid_util::JsEscape(L"a$b"), L"a\\$b");
}

TEST(JsEscape, ControlCharEscaped) {
    // ベル文字 (0x07)
    std::wstring input(1, L'\x07');
    EXPECT_EQ(mermaid_util::JsEscape(input), L"\\u0007");
}

TEST(JsEscape, MermaidDiagramCode) {
    std::wstring code = L"graph TD;\n  A-->B;\n  B-->C;";
    auto escaped = mermaid_util::JsEscape(code);
    // 生の改行を含まないこと
    EXPECT_EQ(escaped.find(L'\n'), std::wstring::npos);
    // エスケープされた改行を含むこと
    EXPECT_NE(escaped.find(L"\\n"), std::wstring::npos);
}

TEST(JsEscape, AllSpecialChars) {
    auto result = mermaid_util::JsEscape(L"\\\'\"\n\r\t`$");
    EXPECT_EQ(result, L"\\\\\\'\\\"\\n\\r\\t\\`\\$");
}

TEST(JsEscape, LineSeparatorEscaped) {
    std::wstring input(1, L'\x2028');
    EXPECT_EQ(mermaid_util::JsEscape(input), L"\\u2028");
}

TEST(JsEscape, ParagraphSeparatorEscaped) {
    std::wstring input(1, L'\x2029');
    EXPECT_EQ(mermaid_util::JsEscape(input), L"\\u2029");
}

// ============================================================
// SimpleHash テスト
// ============================================================

TEST(SimpleHash, EmptyString) {
    auto hash = mermaid_util::SimpleHash(L"");
    EXPECT_EQ(hash.size(), 16u); // 16進数文字16文字
}

TEST(SimpleHash, DeterministicOutput) {
    auto h1 = mermaid_util::SimpleHash(L"hello");
    auto h2 = mermaid_util::SimpleHash(L"hello");
    EXPECT_EQ(h1, h2);
}

TEST(SimpleHash, DifferentInputsDifferentHashes) {
    auto h1 = mermaid_util::SimpleHash(L"hello");
    auto h2 = mermaid_util::SimpleHash(L"world");
    EXPECT_NE(h1, h2);
}

TEST(SimpleHash, HashLength) {
    auto hash = mermaid_util::SimpleHash(L"test input");
    EXPECT_EQ(hash.size(), 16u);
    // 有効な16進数文字であること
    for (wchar_t c : hash) {
        EXPECT_TRUE((c >= L'0' && c <= L'9') || (c >= L'a' && c <= L'f'));
    }
}

TEST(SimpleHash, SingleCharDifference) {
    auto h1 = mermaid_util::SimpleHash(L"abc");
    auto h2 = mermaid_util::SimpleHash(L"abd");
    EXPECT_NE(h1, h2);
}

TEST(SimpleHash, LongInput) {
    std::wstring input(10000, L'a');
    auto hash = mermaid_util::SimpleHash(input);
    EXPECT_EQ(hash.size(), 16u);
}
