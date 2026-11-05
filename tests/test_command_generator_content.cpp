// MockTextMeasurer 経路の CommandGenerator テスト (COM / DirectWrite 不要)。
// entry.text_layout は null なので、構造的なコマンド出力を確かめる。ハイライトや button 配置は
// IDWriteTextFormat / IDWriteTextLayout が必要なため別ファイル
// (test_command_generator_highlight.cpp) で扱う。
#include <gtest/gtest.h>
#include "document_test_helpers.h"
#include "cmd_gen_mock_test_base.h"
#include "test_helpers.h"
#include "ui_constants.h"
#include <array>
#include <cmath>
#include <format>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <string_view>
#include <vector>

namespace {

class CmdGenContentTest : public CmdGenMockTestBase {
protected:
    bool IsQuoteBar(const DrawLineCmd& l) const
    {
        return l.stroke_width == theme_.blockquote_bar_width && l.brush_id == BrushId::BlockquoteBar;
    }

    // text_layout が null なので 1 行目の高さはフォールバック値。bullet 中心は物理ピクセルへスナップされる。
    float ExpectedBulletY(int node) const
    {
        return SnapToPhysicalPixel(cache_.Top(node) + theme_.font_size_body * FALLBACK_LINE_HEIGHT_FACTOR * 0.5f, 1.0f);
    }
};

bool IsVertical(const DrawLineCmd& l)
{
    return std::abs(l.p0.x - l.p1.x) < 0.01f;
}

} // namespace

// ---- Heading 下線 ----

// h1 / h2 だけ entry.height の下に hr 色の下線を 1 本引く。h2 は h1 より細い。
TEST_F(CmdGenContentTest, HeadingUnderlineOnlyForH1AndH2)
{
    struct Case {
        const char* md;
        bool underline;
        float thickness;
    };
    const Case kCases[] = {
        { "# Title", true, theme_.hr_thickness },
        { "## Subtitle", true, theme_.h2_underline_thickness },
        { "### Sub", false, 0.0f },
    };
    EXPECT_LT(theme_.h2_underline_thickness, theme_.hr_thickness);
    for (const auto& c : kCases) {
        SCOPED_TRACE(c.md);
        const auto cmds = Generate(c.md);
        ASSERT_EQ(CountCmd<DrawLineCmd>(cmds), c.underline ? 1 : 0);
        if (c.underline) {
            const auto underline = FindFirst<DrawLineCmd>(cmds);
            EXPECT_EQ(underline->brush_id, BrushId::Hr);
            EXPECT_FLOAT_EQ(underline->stroke_width, c.thickness);
        }
    }
}

// ---- blockquote グループ装飾 ----

// ネストした引用はレベルごとに別 x 座標のバーで描く (PR #156)。外→内→外と続けば、外側 (level=1) はネストを貫通して 1 本のバーになる。
TEST_F(CmdGenContentTest, NestedBlockQuote_OuterBarSpansAcrossInnerNesting)
{
    const auto cmds = Generate("> outer\n> > inner\n>\n> back to outer");

    std::map<float, int> count_by_x;
    for (const auto& c : cmds) {
        if (auto* l = std::get_if<DrawLineCmd>(&c); l && IsQuoteBar(*l)) {
            count_by_x[l->p0.x]++;
        }
    }
    ASSERT_GE(count_by_x.size(), 2u);
    // 最も左の x 座標が level=1 (外側) のバー。連続描画されているなら 1 本のみ。
    EXPECT_EQ(count_by_x.begin()->second, 1)
        << "外側バーはネストを貫通して 1 本連続描画される";
}

// Alert ネスト + 後段: 最外側バーは Alert 色で描画される
TEST_F(CmdGenContentTest, AlertWithNested_OuterBarUsesAlertColor)
{
    const auto cmds = Generate("> [!NOTE]\n> Alert head\n> > inner\n>\n> Alert continues");
    EXPECT_TRUE(FindFirst<DrawLineCmd>(cmds, [](const DrawLineCmd& l) static { return l.brush_id == BrushId::AlertNote; }).has_value())
        << "ネストありの Alert でも最外側バーは Note 色で描画される";
}

// ---- 構造テスト ----

TEST_F(CmdGenContentTest, PushClipAndPopClipArePaired)
{
    auto cmds = Generate("Hello\n\n---\n\nWorld");
    EXPECT_EQ(CountCmd<PushClipCmd>(cmds), 1);
    EXPECT_EQ(CountCmd<PopClipCmd>(cmds), 1);
}

TEST_F(CmdGenContentTest, TransformsArePaired)
{
    auto cmds = Generate("Hello");
    EXPECT_EQ(CountCmd<SetTransformCmd>(cmds), 2); // スクロール変換 + 単位行列リセット
}

// ---- 角丸の背景 ----

// コードブロック・Alert と、ビットマップ未設定の画像・Mermaid のプレースホルダーは角丸の背景を描く。段落は描かない。
// プレースホルダーの背景はコードブロックと同じ色。
TEST_F(CmdGenContentTest, BlocksWithBackgroundGenerateRoundedRect)
{
    struct Case {
        const char* name;
        const char* md;
        BrushId background; // None は背景なし
    };
    constexpr Case cases[] = {
        { "CodeBlock",             "```\ncode\n```",                     BrushId::CodeBg         },
        { "CodeBlockWithLanguage", "```cpp\nint x = 42;\n```",           BrushId::CodeBg         },
        { "Alert",                 "> [!WARNING]\n> Be careful",         BrushId::AlertWarningBg },
        { "ImageWithoutBitmap",    "![alt](image.png)",                  BrushId::CodeBg         },
        { "MermaidWithoutBitmap",  "```mermaid\ngraph TD\n  A-->B\n```", BrushId::CodeBg         },
        { "Paragraph",             "Hello world",                        BrushId::None           },
    };
    for (const auto& c : cases) {
        SCOPED_TRACE(c.name);
        const auto cmds = Generate(c.md);
        if (c.background == BrushId::None) {
            EXPECT_EQ(CountCmd<FillRoundedRectCmd>(cmds), 0);
        }
        else {
            EXPECT_GE(CountCmd<FillRoundedRectCmd>(cmds, [&](const FillRoundedRectCmd& r) { return r.brush_id == c.background; }), 1);
        }
    }
}

// ---- 引用ブロック ----

TEST_F(CmdGenContentTest, MultiLineBlockQuoteGeneratesSingleBar)
{
    auto cmds = Generate("> Line 1\n>\n> Line 2");
    EXPECT_EQ(CountCmd<DrawLineCmd>(cmds, IsVertical), 1) << "複数行の引用ブロックは1本の統合されたバーのみ生成するべき";
}

// Alert のバーと背景は、通常の引用とは別のブラシ (種類の色) で描く。
TEST_F(CmdGenContentTest, MultiLineAlertGeneratesSingleBarAndBackground)
{
    auto cmds = Generate("> [!NOTE]\n> Line 1\n>\n> Line 2");
    EXPECT_EQ(CountCmd<DrawLineCmd>(cmds, IsVertical), 1) << "複数行のAlertは1本の統合されたバーのみ生成するべき";
    EXPECT_EQ(CountCmd<FillRoundedRectCmd>(cmds), 1) << "複数行のAlertは1つの統合された背景のみ生成するべき";
    EXPECT_EQ(CountCmd<DrawLineCmd>(cmds, [](const DrawLineCmd& l) { return IsVertical(l) && l.brush_id == BrushId::AlertNote; }), 1);
    EXPECT_EQ(CountCmd<FillRoundedRectCmd>(cmds, [](const FillRoundedRectCmd& r) static { return r.brush_id == BrushId::AlertNoteBg; }), 1);
}

// ---- 箇条書き記号を持たないリスト ----

TEST_F(CmdGenContentTest, OrderedAndTaskListsGenerateNoBullet)
{
    for (const char* md : { "1. First\n2. Second\n3. Third", "- [x] Done\n- [ ] Not done" }) {
        SCOPED_TRACE(md);
        EXPECT_EQ(CountCmd<FillRoundedRectCmd>(Generate(md), kDisc), 0);
    }
}

// ---- コピーボタン ----

// アイコンのフォントが無ければ、コピーボタンは背景も出さない。
TEST_F(CmdGenContentTest, CodeBlockNoCopyButtonWithoutIconFont)
{
    const auto cmds = Generate("```\ncode\n```");
    EXPECT_EQ(CountCmd<FillRoundedRectCmd>(cmds, [](const FillRoundedRectCmd& r) static { return r.brush_id == BrushId::Overlay; }), 0);
}

// ---- リスト箇条書き記号の垂直位置 ----

TEST_F(CmdGenContentTest, NestedListBulletCenteredOnFirstLine)
{
    auto cmds = Generate("- A\n  - B");
    const float expected_y0 = ExpectedBulletY(0);
    const float expected_y1 = ExpectedBulletY(1);

    const auto disc = FindFirst<FillRoundedRectCmd>(cmds, kDisc);
    const auto circle = FindFirst<DrawRoundedRectCmd>(cmds, kCircle);
    ASSERT_TRUE(disc.has_value());
    ASSERT_TRUE(circle.has_value());
    EXPECT_NEAR(RectCenter(disc->rect).y, expected_y0, 0.01f) << "親リスト項目の箇条書き記号";
    EXPECT_NEAR(RectCenter(circle->rect).y, expected_y1, 0.01f) << "子リスト項目の箇条書き記号";
    EXPECT_EQ(CountCmd<FillRoundedRectCmd>(cmds, kDisc) + CountCmd<DrawRoundedRectCmd>(cmds, kCircle), 2) << "親と子の箇条書き記号が1つずつ存在するべき";
}

// ● ○ ■ □ を段ごとに繰り返す (issue#365, #407)。
TEST_F(CmdGenContentTest, BulletShapeCyclesByListDepth)
{
    const auto cmds = Generate("- 1\n  - 2\n    - 3\n      - 4\n        - 5");
    std::vector<char> shapes;
    for (const auto& cmd : cmds) {
        if (const auto* f = std::get_if<FillRoundedRectCmd>(&cmd); f && kDisc(*f)) {
            shapes.push_back('F');
        }
        else if (const auto* d = std::get_if<DrawRoundedRectCmd>(&cmd); d && kCircle(*d)) {
            shapes.push_back('O');
        }
        else if (const auto* r = std::get_if<FillRectCmd>(&cmd); r && kFilledSquare(*r)) {
            shapes.push_back('S');
        }
        else if (d && kOutlinedSquare(*d)) {
            shapes.push_back('Q');
        }
    }
    EXPECT_EQ(shapes, (std::vector<char>{ 'F', 'O', 'S', 'Q', 'F' }));
}

// □ は ■ と同じ外形にする (線は外形の内側に収める)。
TEST_F(CmdGenContentTest, OutlinedSquareBulletMatchesFilledSquare)
{
    const auto cmds = Generate("- a\n  - b\n    - c\n      - d");
    const auto filled = FindFirst<FillRectCmd>(cmds, kFilledSquare);
    const auto outlined = FindFirst<DrawRoundedRectCmd>(cmds, kOutlinedSquare);
    ASSERT_TRUE(filled.has_value());
    ASSERT_TRUE(outlined.has_value());
    EXPECT_FLOAT_EQ(outlined->radius, 0.0f);
    EXPECT_NEAR(outlined->rect.right - outlined->rect.left + outlined->stroke_width, filled->rect.right - filled->rect.left, 0.01f);
    EXPECT_NEAR(RectCenter(outlined->rect).y, ExpectedBulletY(3), 0.01f);
}

// 四角は円と同じ中心に置き、円より大きくしない。
TEST_F(CmdGenContentTest, SquareBulletCenteredOnFirstLine)
{
    const auto square = FindFirst<FillRectCmd>(Generate("- a\n  - b\n    - c"), kFilledSquare);
    ASSERT_TRUE(square.has_value());
    EXPECT_NEAR(RectCenter(square->rect).y, ExpectedBulletY(2), 0.01f);
    EXPECT_NEAR(square->rect.right - square->rect.left, square->rect.bottom - square->rect.top, 0.01f);
    EXPECT_LE(square->rect.bottom - square->rect.top, theme_.list_bullet_radius * 2.0f);
}

// 引用の中でもリストの最外側は ● (引用で 1 段深く数えていた)。
TEST_F(CmdGenContentTest, ListInBlockQuoteStartsWithDisc)
{
    const auto cmds = Generate("> - a\n>   - b");
    EXPECT_EQ(CountCmd<FillRoundedRectCmd>(cmds, kDisc), 1);
    EXPECT_EQ(CountCmd<DrawRoundedRectCmd>(cmds, kCircle), 1);
}

TEST(FormatListNumberTest, StyleCyclesByDepth)
{
    struct Case {
        int32_t number;
        int depth;
        std::wstring_view expected;
    };
    constexpr Case kCases[] = {
        { 3,                                   1, L"3."               },
        { 3,                                   2, L"iii."             },
        { 3,                                   3, L"c."               },
        { 3,                                   4, L"3."               }, // 記号 (4 種類) と違い 3 種類で繰り返す
        { 3,                                   5, L"iii."             },
        { 3,                                   0, L"3."               }, // 段を持たないノード
        { 4,                                   2, L"iv."              },
        { 1994,                                2, L"mcmxciv."         },
        { 3888,                                2, L"mmmdccclxxxviii." },
        { 26,                                  3, L"z."               },
        { 27,                                  3, L"aa."              },
        { 702,                                 3, L"zz."              },
        { 703,                                 3, L"aaa."             },
        // ローマ数字・アルファベットで表せない番号は 10 進。
        { 0,                                   2, L"0."               },
        { 4000,                                2, L"4000."            },
        { 0,                                   3, L"0."               },
        { std::numeric_limits<int32_t>::max(), 3, L"fxshrxw."         },
        { std::numeric_limits<int32_t>::min(), 1, L"-2147483648."     },
    };
    for (const auto& c : kCases) {
        SCOPED_TRACE(std::format("number={} depth={}", c.number, c.depth));
        std::array<wchar_t, kListNumberMaxLen> buf;
        EXPECT_EQ(FormatListNumber(buf, c.number, c.depth), c.expected);
    }
}

// ---- 区切ったコードブロック ----

// 区切ったコードブロックは間を空けずに並べ、背景は継ぎ目で丸めずにつなげる。
TEST_F(CmdGenContentTest, SplitCodeBlockDrawsContinuousBackground)
{
    const std::string md = MakeCodeBlockMd(5000);
    Parse(md);
    ASSERT_GT(nodes_.size(), 2u);
    for (size_t i = 1; i < nodes_.size(); ++i) {
        EXPECT_FLOAT_EQ(cache_.Top(i), cache_.Top(i - 1) + cache_[i - 1].height);
    }

    // 1 つ目と 2 つ目の継ぎ目を画面の y=200 に置く。
    const float seam = cache_.Top(1);
    const auto cmds = GenerateMdPaneAt(gen_, nodes_, cache_, PaneRect{ 0, 0, kViewportW, 400.0f }, seam - 200.0f);
    const auto code_bg_edge = [&](auto edge) {
        return CountCmd<FillRectCmd>(cmds, [&](const FillRectCmd& c) { return c.brush_id == BrushId::CodeBg && std::abs(edge(c.rect) - 200.0f) < 0.01f; });
    };
    EXPECT_GE(code_bg_edge([](const D2D1_RECT_F& r) static noexcept { return r.bottom; }), 1);
    EXPECT_GE(code_bg_edge([](const D2D1_RECT_F& r) static noexcept { return r.top; }), 1);
}
