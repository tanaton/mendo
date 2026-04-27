#include <gtest/gtest.h>
#include "ascii_util.h"
#include <cwctype>
#include <string>
#include <string_view>

namespace {

// ascii_util::ToLower の意味論をスカラで再現する: ASCII は 'A'-'Z'→'a'-'z' を
// 明示的に行い (locale 非依存)、非 ASCII のみ std::towlower にフォールバックする。
std::wstring ScalarHybridLower(std::wstring_view s)
{
    std::wstring r;
    r.reserve(s.size());
    for (wchar_t ch : s) {
        if (ch >= L'A' && ch <= L'Z') {
            r.push_back(static_cast<wchar_t>(ch - L'A' + L'a'));
        }
        else if (static_cast<unsigned>(ch) < 0x80) {
            r.push_back(ch);
        }
        else {
            r.push_back(static_cast<wchar_t>(std::towlower(ch)));
        }
    }
    return r;
}

std::wstring ScalarAsciiToLower(std::wstring_view s)
{
    std::wstring r;
    r.reserve(s.size());
    for (wchar_t ch : s) {
        r.push_back((ch >= L'A' && ch <= L'Z')
            ? static_cast<wchar_t>(ch - L'A' + L'a')
            : ch);
    }
    return r;
}

} // namespace

// ── ToLower ────────────────────────────────────────────────────

TEST(SimdAsciiToLowerTest, Empty)
{
    std::wstring out;
    ascii_util::ToLower(out.data(), out.data(), 0);
    EXPECT_TRUE(out.empty());
}

TEST(SimdAsciiToLowerTest, AllAsciiVariousLengths)
{
    // 0..40 まで全長で確認 (SSE2 8 文字境界・端数の両方を網羅)
    // ASCII のみのケースは locale 非依存に必ず 'A'-'Z'→'a'-'z' にする契約。
    const std::wstring_view base = L"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghij0123!?@_#";
    for (size_t n = 0; n <= base.size(); ++n) {
        std::wstring src(base.substr(0, n));
        std::wstring dst(n, L'\0');
        ascii_util::ToLower(src.data(), dst.data(), n);
        EXPECT_EQ(dst, ScalarAsciiToLower(src)) << "n=" << n;
    }
}

TEST(SimdAsciiToLowerTest, MixedAsciiAndCjk)
{
    // ASCII 部分は明示変換、非 ASCII (CJK 等) は std::towlower にフォールバック
    const std::wstring src = L"HELLO 世界 World ＡＢＣ ABCDEFGH"; // 全角と半角混在
    std::wstring dst(src.size(), L'\0');
    ascii_util::ToLower(src.data(), dst.data(), src.size());
    EXPECT_EQ(dst, ScalarHybridLower(src));
}

TEST(SimdAsciiToLowerTest, BoundaryAt8)
{
    // 8 文字きっかりで SSE2 ループを 1 回だけ回すケース
    const std::wstring src = L"AaBbCcDd";
    std::wstring dst(src.size(), L'\0');
    ascii_util::ToLower(src.data(), dst.data(), src.size());
    EXPECT_EQ(dst, L"aabbccdd");
}

// ── AsciiToLowerOnly ───────────────────────────────────────────

TEST(SimdAsciiAsciiToLowerOnlyTest, Mixed)
{
    const std::wstring src = L"HELLO_World 123 あ"; // ひらがな「あ」を含む
    std::wstring dst(src.size(), L'\0');
    ascii_util::AsciiToLowerOnly(src.data(), dst.data(), src.size());
    EXPECT_EQ(dst, ScalarAsciiToLower(src));
    // 非 ASCII はそのまま
    EXPECT_EQ(dst.back(), L'あ');
}

TEST(SimdAsciiAsciiToLowerOnlyTest, VariousLengths)
{
    const std::wstring_view base = L"ZyxWvuTsrQpoNmlKjiHgfEdcBa_KEYWORD";
    for (size_t n = 0; n <= base.size(); ++n) {
        std::wstring src(base.substr(0, n));
        std::wstring dst(n, L'\0');
        ascii_util::AsciiToLowerOnly(src.data(), dst.data(), n);
        EXPECT_EQ(dst, ScalarAsciiToLower(src)) << "n=" << n;
    }
}

// ── HasAsciiUpper ──────────────────────────────────────────────

TEST(SimdAsciiHasAsciiUpperTest, Empty)
{
    EXPECT_FALSE(ascii_util::HasAsciiUpper(nullptr, 0));
}

TEST(SimdAsciiHasAsciiUpperTest, AllLower)
{
    const std::wstring s = L"abcdefghijklmnop_xyz";
    EXPECT_FALSE(ascii_util::HasAsciiUpper(s.data(), s.size()));
}

TEST(SimdAsciiHasAsciiUpperTest, OneUpperInSimdRange)
{
    const std::wstring s = L"abcdefghijklmnopQrstuvwx"; // 17 文字目に Q
    EXPECT_TRUE(ascii_util::HasAsciiUpper(s.data(), s.size()));
}

TEST(SimdAsciiHasAsciiUpperTest, OneUpperInTail)
{
    const std::wstring s = L"abcdefghX"; // SIMD 1 回 + 残り 1 文字 (タイル末尾の X)
    EXPECT_TRUE(ascii_util::HasAsciiUpper(s.data(), s.size()));
}

TEST(SimdAsciiHasAsciiUpperTest, NonAsciiOnly)
{
    const std::wstring s = L"あいうえお一二三四"; // 9 文字 CJK
    EXPECT_FALSE(ascii_util::HasAsciiUpper(s.data(), s.size()));
}

TEST(SimdAsciiHasAsciiUpperTest, BracketsOutsideRange)
{
    // '[' (0x5B) と '@' (0x40) は範囲外であることを確認
    const std::wstring s = L"[]@`{|}~";
    EXPECT_FALSE(ascii_util::HasAsciiUpper(s.data(), s.size()));
}

// ── Find ──────────────────────────────────────────────────────

TEST(SimdAsciiFindTest, EmptyQuery)
{
    EXPECT_EQ(ascii_util::Find(L"hello", L"", 0), 0u);
    EXPECT_EQ(ascii_util::Find(L"hello", L"", 3), 3u);
    EXPECT_EQ(ascii_util::Find(L"hello", L"", 5), 5u);
    EXPECT_EQ(ascii_util::Find(L"hello", L"", 6), ascii_util::npos);
}

TEST(SimdAsciiFindTest, EmptyText)
{
    EXPECT_EQ(ascii_util::Find(L"", L"a", 0), ascii_util::npos);
}

TEST(SimdAsciiFindTest, QueryLongerThanText)
{
    EXPECT_EQ(ascii_util::Find(L"abc", L"abcd", 0), ascii_util::npos);
}

TEST(SimdAsciiFindTest, SingleChar)
{
    const std::wstring t = L"abcdefghijklmnopqrstuvwxyz0123456789";
    EXPECT_EQ(ascii_util::Find(t, L"a", 0), 0u);
    EXPECT_EQ(ascii_util::Find(t, L"z", 0), 25u);
    EXPECT_EQ(ascii_util::Find(t, L"9", 0), 35u);
    EXPECT_EQ(ascii_util::Find(t, L"!", 0), ascii_util::npos);
}

TEST(SimdAsciiFindTest, MultiCharBasic)
{
    const std::wstring t = L"the quick brown fox jumps over the lazy dog";
    EXPECT_EQ(ascii_util::Find(t, L"quick", 0), 4u);
    EXPECT_EQ(ascii_util::Find(t, L"the", 0), 0u);
    EXPECT_EQ(ascii_util::Find(t, L"the", 1), 31u);
    EXPECT_EQ(ascii_util::Find(t, L"cat", 0), ascii_util::npos);
}

TEST(SimdAsciiFindTest, AtTextEnd)
{
    const std::wstring t = L"prefix__suffix";
    EXPECT_EQ(ascii_util::Find(t, L"suffix", 0), 8u);
    EXPECT_EQ(ascii_util::Find(t, L"suffix", 8), 8u);
    EXPECT_EQ(ascii_util::Find(t, L"suffix", 9), ascii_util::npos);
}

TEST(SimdAsciiFindTest, PartialPrefixOnly)
{
    // 候補位置に最初の文字だけ一致するが残りが違うケースで誤検出しないこと
    const std::wstring t = L"abXabXabXabXabcXabc";
    EXPECT_EQ(ascii_util::Find(t, L"abc", 0), 12u);
}

TEST(SimdAsciiFindTest, WithCjk)
{
    const std::wstring t = L"検索テスト hello 検索テスト";
    EXPECT_EQ(ascii_util::Find(t, L"hello", 0), 6u);
    EXPECT_EQ(ascii_util::Find(t, L"テスト", 0), 2u); // テスト
    EXPECT_EQ(ascii_util::Find(t, L"テスト", 3), 14u);
}

TEST(SimdAsciiFindTest, CrossingSimdBoundary)
{
    // SIMD 境界 (8 文字単位) を跨ぐマッチを発生させる
    std::wstring t(20, L'a');
    t.replace(7, 3, L"xyz"); // 位置 7..9 に xyz (8 文字目を跨ぐ)
    EXPECT_EQ(ascii_util::Find(t, L"xyz", 0), 7u);
    EXPECT_EQ(ascii_util::Find(t, L"axyz", 0), 6u);
}

TEST(SimdAsciiFindTest, MatchEqualsTextLen)
{
    const std::wstring t = L"hello";
    EXPECT_EQ(ascii_util::Find(t, L"hello", 0), 0u);
    EXPECT_EQ(ascii_util::Find(t, L"hello", 1), ascii_util::npos);
}

TEST(SimdAsciiFindTest, RepeatedMatches)
{
    const std::wstring t = L"aaaaaaaaaaaaaaaaaa"; // 18 個の 'a'
    EXPECT_EQ(ascii_util::Find(t, L"aa", 0), 0u);
    EXPECT_EQ(ascii_util::Find(t, L"aa", 5), 5u);
    EXPECT_EQ(ascii_util::Find(t, L"aaa", 0), 0u);
    EXPECT_EQ(ascii_util::Find(t, L"aaa", 15), 15u);
    EXPECT_EQ(ascii_util::Find(t, L"aaa", 16), ascii_util::npos);
}

TEST(SimdAsciiFindTest, StartBeyondLast)
{
    const std::wstring t = L"hello";
    // start > tlen - qlen のケース
    EXPECT_EQ(ascii_util::Find(t, L"lo", 4), ascii_util::npos);
    EXPECT_EQ(ascii_util::Find(t, L"lo", 3), 3u);
}
