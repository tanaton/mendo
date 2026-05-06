#include <gtest/gtest.h>
#include "doc_dwrite_bridge.h"

using mendo::WideViewForDWrite;

// ─────────────────────────────────────────────
// WideOffsetFromDocOffset / DocOffsetFromWideOffset
// ─────────────────────────────────────────────

TEST(WideViewForDWrite, EmptyText)
{
    WideViewForDWrite wv{ "" };
    EXPECT_TRUE(wv.wide().empty());
    EXPECT_EQ(wv.WideOffsetFromDocOffset(0), 0u);
    EXPECT_EQ(wv.DocOffsetFromWideOffset(0), 0u);
}

TEST(WideViewForDWrite, AsciiOnly)
{
    WideViewForDWrite wv{ "Hello" };
    EXPECT_EQ(wv.wide(), L"Hello");
    for (uint32_t i = 0; i <= 5; ++i) {
        EXPECT_EQ(wv.WideOffsetFromDocOffset(i), i);
        EXPECT_EQ(wv.DocOffsetFromWideOffset(i), i);
    }
}

TEST(WideViewForDWrite, Cjk3ByteRoundTrip)
{
    // テスト = 3 文字, UTF-8 9 bytes, UTF-16 3 wide units
    WideViewForDWrite wv{ "テスト" };
    EXPECT_EQ(wv.wide(), L"テスト");

    // doc → wide: leading byte の累積 wide 数
    EXPECT_EQ(wv.WideOffsetFromDocOffset(0), 0u);
    EXPECT_EQ(wv.WideOffsetFromDocOffset(3), 1u);
    EXPECT_EQ(wv.WideOffsetFromDocOffset(6), 2u);
    EXPECT_EQ(wv.WideOffsetFromDocOffset(9), 3u);

    // wide → doc: 対応する文字の leading byte 位置 (= continuation byte は返さない)
    EXPECT_EQ(wv.DocOffsetFromWideOffset(0), 0u);
    EXPECT_EQ(wv.DocOffsetFromWideOffset(1), 3u);
    EXPECT_EQ(wv.DocOffsetFromWideOffset(2), 6u);
    EXPECT_EQ(wv.DocOffsetFromWideOffset(3), 9u);
}

TEST(WideViewForDWrite, Latin2ByteRoundTrip)
{
    // é = U+00E9 (UTF-8 2 bytes, UTF-16 1 wide unit), あいだに ASCII を挟む
    WideViewForDWrite wv{ "aébc" };
    EXPECT_EQ(wv.wide(), L"aébc");

    // 'a'=0..1, 'é'=1..3, 'b'=3..4, 'c'=4..5 (utf8 byte / utf16 wide)
    EXPECT_EQ(wv.WideOffsetFromDocOffset(0), 0u);
    EXPECT_EQ(wv.WideOffsetFromDocOffset(1), 1u);
    EXPECT_EQ(wv.WideOffsetFromDocOffset(3), 2u);
    EXPECT_EQ(wv.WideOffsetFromDocOffset(4), 3u);
    EXPECT_EQ(wv.WideOffsetFromDocOffset(5), 4u);

    EXPECT_EQ(wv.DocOffsetFromWideOffset(0), 0u);
    EXPECT_EQ(wv.DocOffsetFromWideOffset(1), 1u);
    EXPECT_EQ(wv.DocOffsetFromWideOffset(2), 3u);
    EXPECT_EQ(wv.DocOffsetFromWideOffset(3), 4u);
    EXPECT_EQ(wv.DocOffsetFromWideOffset(4), 5u);
}

TEST(WideViewForDWrite, SurrogatePairRoundTrip)
{
    // 😀 = U+1F600 (UTF-8 4 bytes, UTF-16 2 wide units = surrogate pair)
    WideViewForDWrite wv{ "A\xF0\x9F\x98\x80""B" };
    EXPECT_EQ(wv.wide().size(), 4u); // 'A' + high + low + 'B'

    // doc → wide
    EXPECT_EQ(wv.WideOffsetFromDocOffset(0), 0u);
    EXPECT_EQ(wv.WideOffsetFromDocOffset(1), 1u); // 絵文字 leading byte
    EXPECT_EQ(wv.WideOffsetFromDocOffset(5), 3u); // 'B' leading byte
    EXPECT_EQ(wv.WideOffsetFromDocOffset(6), 4u);

    // wide=2 (サロゲートペア中央) は絵文字 leading (1) ではなく次文字の leading (5)。
    EXPECT_EQ(wv.DocOffsetFromWideOffset(0), 0u);
    EXPECT_EQ(wv.DocOffsetFromWideOffset(1), 1u);
    EXPECT_EQ(wv.DocOffsetFromWideOffset(2), 5u);
    EXPECT_EQ(wv.DocOffsetFromWideOffset(3), 5u);
    EXPECT_EQ(wv.DocOffsetFromWideOffset(4), 6u);
}

// 回帰テスト: #183
// テキスト末尾の文字を選択した際、HitTest で is_trailing=true により
// wide_off = (文字数) - 1 + 1 = wide_size に達する直前の値が渡される。
// 旧実装は continuation byte の index を返してしまい、ExtractSelectedText で
// UTF-8 として不正な末尾バイト列が抽出され、貼り付け先で文字化けしていた。
TEST(WideViewForDWrite, EndOfNonAsciiCharNeverReturnsContinuationByte)
{
    WideViewForDWrite wv{ "テスト" };
    // 末尾「ト」の手前 (= 「ス」の右端) を指す wide_off=2 は
    // 「ト」の leading byte (= 6) を返さなければならない。
    EXPECT_EQ(wv.DocOffsetFromWideOffset(2), 6u);

    // 「テ」の右端 (= 「ス」の左端) は byte 3 (= 「ス」の leading byte)
    EXPECT_EQ(wv.DocOffsetFromWideOffset(1), 3u);
}

TEST(WideViewForDWrite, OutOfRangeWideOffsetClampsToBack)
{
    WideViewForDWrite wv{ "テスト" };
    EXPECT_EQ(wv.DocOffsetFromWideOffset(3), 9u);    // 番兵
    EXPECT_EQ(wv.DocOffsetFromWideOffset(100), 9u);  // 範囲外も clamping
}

TEST(WideViewForDWrite, OutOfRangeDocOffsetClampsToBack)
{
    WideViewForDWrite wv{ "テスト" };
    EXPECT_EQ(wv.WideOffsetFromDocOffset(9), 3u);    // 番兵
    EXPECT_EQ(wv.WideOffsetFromDocOffset(100), 3u);  // 範囲外も clamping
}

TEST(WideViewForDWrite, WideRangeForCjk)
{
    WideViewForDWrite wv{ "テスト" };
    // [3, 6) byte = 「ス」 = wide [1, 2)
    const auto r = wv.WideRange(3, 3);
    EXPECT_EQ(r.startPosition, 1u);
    EXPECT_EQ(r.length, 1u);
}
