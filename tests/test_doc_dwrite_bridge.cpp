#include <gtest/gtest.h>
#include "doc_dwrite_bridge.h"

namespace {

uint32_t WideAt(std::string_view text, uint32_t byte)
{
    return Utf16OffsetCursor{ text }.WideAt(byte);
}

uint32_t ByteAt(std::string_view text, uint32_t wide)
{
    return Utf16OffsetCursor{ text }.ByteAt(wide);
}

std::pmr::wstring Decode(std::string_view text)
{
    std::pmr::wstring out;
    DecodeForDWrite(text, out);
    return out;
}

} // namespace

// ─────────────────────────────────────────────
// Utf16OffsetCursor: WideAt (byte → wide) / ByteAt (wide → byte)
// ─────────────────────────────────────────────

TEST(Utf16OffsetCursor, EmptyText)
{
    EXPECT_EQ(WideAt("", 0), 0u);
    EXPECT_EQ(ByteAt("", 0), 0u);
}

TEST(Utf16OffsetCursor, AsciiOnly)
{
    for (uint32_t i = 0; i <= 5; ++i) {
        EXPECT_EQ(WideAt("Hello", i), i);
        EXPECT_EQ(ByteAt("Hello", i), i);
    }
}

TEST(Utf16OffsetCursor, AsciiOnlyOutOfRangeClampsToSize)
{
    EXPECT_EQ(WideAt("Hello", 100), 5u);
    EXPECT_EQ(ByteAt("Hello", 100), 5u);
}

TEST(Utf16OffsetCursor, AsciiPrefixBeforeNonAscii)
{
    // "日" は 3 bytes / 1 wide
    const std::string_view text = "ab日c";
    EXPECT_EQ(WideAt(text, 1), 1u);
    EXPECT_EQ(WideAt(text, 2), 2u);
    EXPECT_EQ(WideAt(text, 5), 3u);
    EXPECT_EQ(WideAt(text, 6), 4u);
    EXPECT_EQ(ByteAt(text, 3), 5u);
}

TEST(Utf16OffsetCursor, Cjk3ByteRoundTrip)
{
    // テスト = 3 文字, UTF-8 9 bytes, UTF-16 3 wide units
    const std::string_view text = "テスト";

    EXPECT_EQ(WideAt(text, 0), 0u);
    EXPECT_EQ(WideAt(text, 3), 1u);
    EXPECT_EQ(WideAt(text, 6), 2u);
    EXPECT_EQ(WideAt(text, 9), 3u);

    // wide → byte: 対応する文字の leading byte 位置 (= continuation byte は返さない)
    EXPECT_EQ(ByteAt(text, 0), 0u);
    EXPECT_EQ(ByteAt(text, 1), 3u);
    EXPECT_EQ(ByteAt(text, 2), 6u);
    EXPECT_EQ(ByteAt(text, 3), 9u);
}

TEST(Utf16OffsetCursor, Latin2ByteRoundTrip)
{
    // é = U+00E9 (UTF-8 2 bytes, UTF-16 1 wide unit)
    // 'a'=0..1, 'é'=1..3, 'b'=3..4, 'c'=4..5 (utf8 byte / utf16 wide)
    const std::string_view text = "aébc";
    EXPECT_EQ(WideAt(text, 0), 0u);
    EXPECT_EQ(WideAt(text, 1), 1u);
    EXPECT_EQ(WideAt(text, 3), 2u);
    EXPECT_EQ(WideAt(text, 4), 3u);
    EXPECT_EQ(WideAt(text, 5), 4u);

    EXPECT_EQ(ByteAt(text, 0), 0u);
    EXPECT_EQ(ByteAt(text, 1), 1u);
    EXPECT_EQ(ByteAt(text, 2), 3u);
    EXPECT_EQ(ByteAt(text, 3), 4u);
    EXPECT_EQ(ByteAt(text, 4), 5u);
}

TEST(Utf16OffsetCursor, SurrogatePairRoundTrip)
{
    // 😀 = U+1F600 (UTF-8 4 bytes, UTF-16 2 wide units = surrogate pair)
    const std::string_view text = "A\xF0\x9F\x98\x80""B";

    EXPECT_EQ(WideAt(text, 0), 0u);
    EXPECT_EQ(WideAt(text, 1), 1u); // 絵文字 leading byte
    EXPECT_EQ(WideAt(text, 5), 3u); // 'B' leading byte
    EXPECT_EQ(WideAt(text, 6), 4u);

    // wide=2 (サロゲートペア中央) は絵文字 leading (1) ではなく次文字の leading (5)。
    EXPECT_EQ(ByteAt(text, 0), 0u);
    EXPECT_EQ(ByteAt(text, 1), 1u);
    EXPECT_EQ(ByteAt(text, 2), 5u);
    EXPECT_EQ(ByteAt(text, 3), 5u);
    EXPECT_EQ(ByteAt(text, 4), 6u);
}

// 回帰テスト: #183
// テキスト末尾の文字を選択した際、HitTest で is_trailing=true により
// wide_off = (文字数) - 1 + 1 = wide_size に達する直前の値が渡される。
// 旧実装は continuation byte の index を返してしまい、ExtractSelectedText で
// UTF-8 として不正な末尾バイト列が抽出され、貼り付け先で文字化けしていた。
TEST(Utf16OffsetCursor, EndOfNonAsciiCharNeverReturnsContinuationByte)
{
    // 末尾「ト」の手前 (= 「ス」の右端) を指す wide=2 は「ト」の leading byte (= 6)。
    EXPECT_EQ(ByteAt("テスト", 2), 6u);
    // 「テ」の右端 (= 「ス」の左端) は byte 3 (= 「ス」の leading byte)
    EXPECT_EQ(ByteAt("テスト", 1), 3u);
}

TEST(Utf16OffsetCursor, OutOfRangeClampsToBack)
{
    EXPECT_EQ(ByteAt("テスト", 3), 9u);
    EXPECT_EQ(ByteAt("テスト", 100), 9u);
    EXPECT_EQ(WideAt("テスト", 9), 3u);
    EXPECT_EQ(WideAt("テスト", 100), 3u);
}

TEST(Utf16OffsetCursor, WideRangeForCjk)
{
    // [3, 6) byte = 「ス」 = wide [1, 2)
    Utf16OffsetCursor cursor{ "テスト" };
    const auto r = cursor.WideRange(3, 3);
    EXPECT_EQ(r.startPosition, 1u);
    EXPECT_EQ(r.length, 1u);
}

// 前方へ進めたカーソルの結果は毎回作り直したカーソルと一致し (ASCII 16 byte の読み飛ばしを含む)、
// 全 byte 位置で DecodeForDWrite の変換と整合する (不正バイト含む)。
TEST(Utf16OffsetCursor, ConsistentWithDecodeForEveryOffset)
{
    std::string text = "ascii \xE3\x81\x82\xE3\x81\x84 \xF0\x9F\x98\x80 bad\xE3\x81 \xFF\x80 end";
    for (int i = 0; i < 3; ++i) {
        text += "0123456789abcdef0123456789";
    }
    text += "\xE6\x97\xA5";
    const auto wide = Decode(text);

    Utf16OffsetCursor forward{ text };
    for (uint32_t b = 0; b <= text.size(); ++b) {
        const uint32_t w = forward.WideAt(b);
        EXPECT_EQ(w, WideAt(text, b)) << "byte=" << b;
        // ByteAt は b を含む文字の先頭 byte を返し、そこから WideAt で同じ位置に戻る。
        const uint32_t lead = ByteAt(text, w);
        EXPECT_LE(lead, b) << "byte=" << b;
        EXPECT_EQ(WideAt(text, lead), w) << "byte=" << b;
    }
    EXPECT_EQ(WideAt(text, static_cast<uint32_t>(text.size())), wide.size());

    Utf16OffsetCursor forward_wide{ text };
    for (uint32_t w = 0; w <= wide.size(); ++w) {
        EXPECT_EQ(forward_wide.ByteAt(w), ByteAt(text, w)) << "wide=" << w;
    }
}

TEST(Utf16OffsetCursor, SkipsForwardWithoutVisitingEveryOffset)
{
    const std::string text = std::string(100, 'a') + "\xE3\x81\x82" + std::string(40, 'b');
    Utf16OffsetCursor cursor{ text };
    EXPECT_EQ(cursor.WideAt(50), 50u);
    EXPECT_EQ(cursor.WideAt(103), 101u);
    EXPECT_EQ(cursor.WideAt(static_cast<uint32_t>(text.size())), 141u);

    Utf16OffsetCursor back{ text };
    EXPECT_EQ(back.ByteAt(50), 50u);
    EXPECT_EQ(back.ByteAt(101), 103u);
    EXPECT_EQ(back.ByteAt(141), static_cast<uint32_t>(text.size()));
}

// ─────────────────────────────────────────────
// DecodeForDWrite
// ─────────────────────────────────────────────

TEST(DecodeForDWrite, DecodesText)
{
    EXPECT_TRUE(Decode("").empty());
    EXPECT_EQ(Decode("Hello"), L"Hello");
    EXPECT_EQ(Decode("ab日c"), L"ab日c");
    EXPECT_EQ(Decode("A\xF0\x9F\x98\x80""B"), L"A\xD83D\xDE00""B");
}

// 不正バイトは U+FFFD 1 単位。
TEST(DecodeForDWrite, InvalidBytesBecomeReplacementChar)
{
    EXPECT_EQ(Decode("a\xFF""b"), L"a\xFFFD""b");
}

// 使い回すバッファに前回の内容は残らない。
TEST(DecodeForDWrite, ReplacesPreviousContent)
{
    std::pmr::wstring out;
    DecodeForDWrite("longer text", out);
    DecodeForDWrite("ab", out);
    EXPECT_EQ(out, L"ab");
    DecodeForDWrite("", out);
    EXPECT_TRUE(out.empty());
}
