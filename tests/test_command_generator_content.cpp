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
// h1 / h2 のみ entry.height の下に下線が描画される（hr_color）。
// h3 以降では下線が出ない。

TEST_F(CmdGenContentTest, HeadingH1_EmitsUnderlineWithHrColor)
{
    const auto cmds = Generate("# Title");
    ASSERT_EQ(CountCmd<DrawLineCmd>(cmds), 1) << "h1 は下線を 1 本発行する";
    const auto underline = FindFirst<DrawLineCmd>(cmds);
    EXPECT_EQ(underline->brush_id, BrushId::Hr);
    EXPECT_FLOAT_EQ(underline->stroke_width, theme_.hr_thickness);
}

TEST_F(CmdGenContentTest, HeadingH2_EmitsUnderlineWithCorrectThickness)
{
    const auto cmds = Generate("## Subtitle");
    ASSERT_EQ(CountCmd<DrawLineCmd>(cmds), 1) << "h2 は下線を 1 本発行する";
    const auto underline = FindFirst<DrawLineCmd>(cmds);
    EXPECT_EQ(underline->brush_id, BrushId::Hr);
    EXPECT_FLOAT_EQ(underline->stroke_width, theme_.h2_underline_thickness);
    EXPECT_LT(underline->stroke_width, theme_.hr_thickness);
}

TEST_F(CmdGenContentTest, HeadingH3_DoesNotEmitUnderline)
{
    EXPECT_EQ(CountCmd<DrawLineCmd>(Generate("### Sub")), 0) << "h3 は下線を発行しない";
}

// ---- blockquote グループ装飾 ----

TEST_F(CmdGenContentTest, BlockQuote_EmitsBarLine)
{
    const auto cmds = Generate("> Quoted");
    EXPECT_GE(CountCmd<DrawLineCmd>(cmds, [&](const DrawLineCmd& l) { return IsQuoteBar(l); }), 1)
        << "blockquote には bar_color の縦バーが 1 本以上出る";
}

TEST_F(CmdGenContentTest, BlockQuote_FullyAboveViewport_NoBar)
{
    Parse("> Quoted");
    const PaneRect pane{ 0.0f, 0.0f, 800.0f, 50.0f };
    // viewport_top を blockquote の下端より十分大きくする
    DrawCommandList cmds;
    gen_.GenerateMdPane(cmds, { .nodes = nodes_, .cache = cache_, .selection = TextSelection{}, .md_pane_rect = pane, .scroll_y = 100000.0f }, 1.0f);

    EXPECT_EQ(CountCmd<DrawLineCmd>(cmds), 0) << "viewport より上にあるグループは bar が描画されない";
}

// ネスト blockquote の各レベルを別 x 座標のバーで描画する (PR #156)
TEST_F(CmdGenContentTest, NestedBlockQuote_EmitsBarPerLevel)
{
    const auto cmds = Generate("> outer\n> > inner");

    std::set<float> bar_x_positions;
    for (const auto& c : cmds) {
        if (auto* l = std::get_if<DrawLineCmd>(&c); l && IsQuoteBar(*l)) {
            bar_x_positions.insert(l->p0.x);
        }
    }
    EXPECT_GE(bar_x_positions.size(), 2u)
        << "2 段ネストではレベルごとに別 x 座標のバーが出る";
}

// 外→内→外の連続描画: 外側 (level=1) はネストを貫通して 1 本のバーになる
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

// ---- 複数 hr のうち先頭だけ可視なケース ----
// 上方向カリングと下方向カリングが両端で同時に効くケース。

TEST_F(CmdGenContentTest, HorizontalRule_PartialViewportLimitsToVisible)
{
    Parse("---\n\n---\n\n---\n\n---\n\n---");
    ASSERT_FALSE(nodes_.empty());

    // 1 本目の hr の下端より少し大きい viewport を用意する。
    const float first_hr_bottom = cache_.Top(0) + cache_[0].height;
    const PaneRect pane{ 0.0f, 0.0f, 800.0f, first_hr_bottom + 1.0f };
    DrawCommandList cmds;
    gen_.GenerateMdPane(cmds, { .nodes = nodes_, .cache = cache_, .selection = TextSelection{}, .md_pane_rect = pane }, 1.0f);

    const auto draw_lines = CountCmd<DrawLineCmd>(cmds);
    EXPECT_GE(draw_lines, 1) << "1 本目は確実に可視範囲内";
    EXPECT_LT(draw_lines, 5) << "全 5 本は描画されない（後続はカリングされる）";
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

// コードブロック・Alert と、ビットマップ未設定の画像・Mermaid のプレースホルダーは角丸の背景を描く。
TEST_F(CmdGenContentTest, BlocksWithBackgroundGenerateRoundedRect)
{
    struct Case {
        const char* name;
        const char* md;
    };
    constexpr Case cases[] = {
        { "CodeBlock", "```\ncode\n```" },
        { "CodeBlockWithLanguage", "```cpp\nint x = 42;\n```" },
        { "Alert", "> [!WARNING]\n> Be careful" },
        { "ImageWithoutBitmap", "![alt](image.png)" },
        { "MermaidWithoutBitmap", "```mermaid\ngraph TD\n  A-->B\n```" },
    };
    for (const auto& c : cases) {
        SCOPED_TRACE(c.name);
        EXPECT_GE(CountCmd<FillRoundedRectCmd>(Generate(c.md)), 1);
    }
}

// ---- テーブル ----

TEST_F(CmdGenContentTest, TableGeneratesLinesAndRects)
{
    auto cmds = Generate("| A | B |\n|---|---|\n| 1 | 2 |");
    EXPECT_GT(CountCmd<DrawLineCmd>(cmds), 0);
    EXPECT_GT(CountCmd<FillRectCmd>(cmds), 0);
}

// ---- 引用ブロック ----

TEST_F(CmdGenContentTest, MultiLineBlockQuoteGeneratesSingleBar)
{
    auto cmds = Generate("> Line 1\n>\n> Line 2");
    EXPECT_EQ(CountCmd<DrawLineCmd>(cmds, IsVertical), 1) << "複数行の引用ブロックは1本の統合されたバーのみ生成するべき";
}

TEST_F(CmdGenContentTest, MultiLineAlertGeneratesSingleBarAndBackground)
{
    auto cmds = Generate("> [!NOTE]\n> Line 1\n>\n> Line 2");
    EXPECT_EQ(CountCmd<DrawLineCmd>(cmds, IsVertical), 1) << "複数行のAlertは1本の統合されたバーのみ生成するべき";
    EXPECT_EQ(CountCmd<FillRoundedRectCmd>(cmds), 1) << "複数行のAlertは1つの統合された背景のみ生成するべき";
}

// ---- 複数ノード ----

TEST_F(CmdGenContentTest, MixedContentGeneratesVariousCommands)
{
    auto cmds = Generate("# Heading\n\nParagraph\n\n---\n\n- List\n\n> Quote\n\n```\ncode\n```");
    EXPECT_GT(CountCmd<DrawLineCmd>(cmds), 0);
    EXPECT_GT(CountCmd<FillEllipseCmd>(cmds), 0);
    EXPECT_GT(CountCmd<FillRoundedRectCmd>(cmds), 0);
}

// ---- 箇条書き記号を持たないリスト ----

TEST_F(CmdGenContentTest, OrderedAndTaskListsGenerateNoEllipse)
{
    for (const char* md : { "1. First\n2. Second\n3. Third", "- [x] Done\n- [ ] Not done" }) {
        SCOPED_TRACE(md);
        EXPECT_EQ(CountCmd<FillEllipseCmd>(Generate(md)), 0);
    }
}

// ---- 水平線 ----

TEST_F(CmdGenContentTest, MultipleHorizontalRulesGenerateHrLines)
{
    auto cmds = Generate("---\n\n---\n\n---");
    EXPECT_GE(CountCmd<DrawLineCmd>(cmds, [](const DrawLineCmd& l) static { return l.brush_id == BrushId::Hr; }), 3);
}

// ---- ダークテーマテスト ----

TEST_F(CmdGenContentTest, DarkThemeTableGeneratesCommands)
{
    theme_ = GetDarkTheme();
    gen_.Init(theme_);
    ASSERT_TRUE(engine_.Init(&mock_, theme_));

    auto cmds = Generate("| A | B |\n|---|---|\n| 1 | 2 |");
    EXPECT_GT(CountCmd<DrawLineCmd>(cmds), 0);
}

// ---- GitHub Alerts ----

TEST_F(CmdGenContentTest, AlertBarColorMatchesTheme)
{
    // Note のバーは通常の blockquote のバーとは別のブラシで描く。
    auto cmds = Generate("> [!NOTE]\n> text");
    const auto bar = FindFirst<DrawLineCmd>(cmds, [&](const DrawLineCmd& l) {
        return IsVertical(l) && l.brush_id == BrushId::AlertNote;
    });
    EXPECT_TRUE(bar.has_value()) << "Note のバーは AlertNote ブラシで描くべき";
    const auto bg = FindFirst<FillRoundedRectCmd>(cmds, [](const FillRoundedRectCmd& r) static { return r.brush_id == BrushId::AlertNoteBg; });
    EXPECT_TRUE(bg.has_value()) << "Note の背景は AlertNoteBg ブラシで描くべき";
}

TEST_F(CmdGenContentTest, AllAlertTypesGenerateCommands)
{
    struct AlertCase {
        const char* name;
        const char* md;
    };
    constexpr AlertCase alerts[] = {
        { "NOTE",      "> [!NOTE]\n> n"      },
        { "TIP",       "> [!TIP]\n> t"       },
        { "IMPORTANT", "> [!IMPORTANT]\n> i" },
        { "WARNING",   "> [!WARNING]\n> w"   },
        { "CAUTION",   "> [!CAUTION]\n> c"   },
    };
    for (const auto& a : alerts) {
        SCOPED_TRACE(a.name);
        EXPECT_GE(CountCmd<DrawLineCmd>(Generate(a.md)), 1);
    }
}

// ---- コピーボタン ----

TEST_F(CmdGenContentTest, CodeBlockNoCopyButtonWithoutIconFont)
{
    auto cmds = Generate("```\ncode\n```");
    EXPECT_EQ(CountCmd<DrawTextLayoutCmd>(cmds), 0) << "formats_.copy_btn_icon が null のときコピーボタンのアイコンは生成されないべき";
}

TEST_F(CmdGenContentTest, NonCodeBlockNoCopyButton)
{
    auto cmds = Generate("Hello world");
    EXPECT_EQ(CountCmd<FillRoundedRectCmd>(cmds), 0);
}

// ---- リスト箇条書き記号の垂直位置 ----

TEST_F(CmdGenContentTest, UnorderedListBulletCenteredOnFirstLine)
{
    const auto bullet = FindFirst<FillEllipseCmd>(Generate("- Item"));
    ASSERT_TRUE(bullet.has_value()) << "FillEllipseCmd が見つからない";
    EXPECT_NEAR(bullet->center.y, ExpectedBulletY(0), 0.01f);
}

TEST_F(CmdGenContentTest, NestedListBulletCenteredOnFirstLine)
{
    auto cmds = Generate("- A\n  - B");
    const float expected_y0 = ExpectedBulletY(0);
    const float expected_y1 = ExpectedBulletY(1);

    int idx = 0;
    for (const auto& cmd : cmds) {
        if (auto* e = std::get_if<FillEllipseCmd>(&cmd)) {
            EXPECT_NEAR(e->center.y, expected_y0, 0.01f) << "親リスト項目の箇条書き記号";
            idx++;
        }
        else if (auto* d = std::get_if<DrawEllipseCmd>(&cmd)) {
            EXPECT_NEAR(d->center.y, expected_y1, 0.01f) << "子リスト項目の箇条書き記号";
            idx++;
        }
    }
    EXPECT_EQ(idx, 2) << "親と子の箇条書き記号が1つずつ存在するべき";
}

// ● ○ ■ を段ごとに繰り返す (issue#365)。
TEST_F(CmdGenContentTest, BulletShapeCyclesByListDepth)
{
    const auto cmds = Generate("- 1\n  - 2\n    - 3\n      - 4");
    std::vector<char> shapes;
    for (const auto& cmd : cmds) {
        if (std::holds_alternative<FillEllipseCmd>(cmd)) {
            shapes.push_back('F');
        }
        else if (std::holds_alternative<DrawEllipseCmd>(cmd)) {
            shapes.push_back('O');
        }
        else if (const auto* r = std::get_if<FillRectCmd>(&cmd); r && r->brush_id == BrushId::Text) {
            shapes.push_back('S');
        }
    }
    EXPECT_EQ(shapes, (std::vector<char>{ 'F', 'O', 'S', 'F' }));
}

// 四角は円と同じ中心に置き、円より大きくしない。
TEST_F(CmdGenContentTest, SquareBulletCenteredOnFirstLine)
{
    const auto square = FindFirst<FillRectCmd>(Generate("- a\n  - b\n    - c"), [](const FillRectCmd& r) static { return r.brush_id == BrushId::Text; });
    ASSERT_TRUE(square.has_value());
    EXPECT_NEAR((square->rect.top + square->rect.bottom) * 0.5f, ExpectedBulletY(2), 0.01f);
    EXPECT_NEAR(square->rect.right - square->rect.left, square->rect.bottom - square->rect.top, 0.01f);
    EXPECT_LE(square->rect.bottom - square->rect.top, theme_.list_bullet_radius * 2.0f);
}

// 引用の中でもリストの最外側は ● (引用で 1 段深く数えていた)。
TEST_F(CmdGenContentTest, ListInBlockQuoteStartsWithDisc)
{
    const auto cmds = Generate("> - a\n>   - b");
    EXPECT_EQ(CountCmd<FillEllipseCmd>(cmds), 1);
    EXPECT_EQ(CountCmd<DrawEllipseCmd>(cmds), 1);
}

TEST(FormatListNumberTest, StyleCyclesByDepth)
{
    struct Case {
        int32_t number;
        int depth;
        std::wstring_view expected;
    };
    constexpr Case kCases[] = {
        { 3,    1, L"3."               },
        { 3,    2, L"iii."             },
        { 3,    3, L"c."               },
        { 3,    4, L"3."               },
        { 3,    0, L"3."               }, // 段を持たないノード
        { 4,    2, L"iv."              },
        { 1994, 2, L"mcmxciv."         },
        { 3888, 2, L"mmmdccclxxxviii." },
        { 26,   3, L"z."               },
        { 27,   3, L"aa."              },
        { 702,  3, L"zz."              },
        { 703,  3, L"aaa."             },
        // ローマ数字・アルファベットで表せない番号は 10 進。
        { 0,    2, L"0."               },
        { 4000, 2, L"4000."            },
        { 0,    3, L"0."               },
        { std::numeric_limits<int32_t>::max(), 3, L"fxshrxw." },
        { std::numeric_limits<int32_t>::min(), 1, L"-2147483648." },
    };
    for (const auto& c : kCases) {
        SCOPED_TRACE(std::format("number={} depth={}", c.number, c.depth));
        std::array<wchar_t, kListNumberMaxLen> buf;
        EXPECT_EQ(FormatListNumber(buf, c.number, c.depth), c.expected);
    }
}

// OverlayButtonHit パラメータが GenerateMdPane に渡せることの検証
TEST_F(CmdGenContentTest, CodeBlockWithHoveredCopyNodeAccepted)
{
    Parse("```\ncode\n```");
    const PaneRect md_pane{ 0, 0, 800.0f, 2000.0f };
    // ノード 0 のコピーボタンのホバーを渡してもクラッシュしない
    DrawCommandList cmds;
    gen_.GenerateMdPane(cmds, { .nodes = nodes_, .cache = cache_, .selection = TextSelection{}, .md_pane_rect = md_pane, .hovered = OverlayButtonHit{ 0, OverlayButton::CodeCopy } }, 1.0f);
    EXPECT_TRUE(std::holds_alternative<PushClipCmd>(cmds.front()));
    EXPECT_TRUE(std::holds_alternative<PopClipCmd>(cmds.back()));
}

// ---- 図・画像プレースホルダー ----

TEST_F(CmdGenContentTest, PlaceholderBgUsesCodeBgColor)
{
    auto cmds = Generate("![alt](image.png)");
    const auto rr = FindFirst<FillRoundedRectCmd>(cmds);
    ASSERT_TRUE(rr.has_value()) << "プレースホルダーの角丸四角形が見つからない";
    EXPECT_EQ(rr->brush_id, BrushId::CodeBg);
}

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
    DrawCommandList cmds;
    gen_.GenerateMdPane(cmds, { .nodes = nodes_, .cache = cache_, .selection = TextSelection{}, .md_pane_rect = PaneRect{ 0, 0, kViewportW, 400.0f }, .scroll_y = seam - 200.0f }, 1.0f);
    const auto code_bg_edge = [&](auto edge) {
        return CountCmd<FillRectCmd>(cmds, [&](const FillRectCmd& c) { return c.brush_id == BrushId::CodeBg && std::abs(edge(c.rect) - 200.0f) < 0.01f; });
    };
    EXPECT_GE(code_bg_edge([](const D2D1_RECT_F& r) { return r.bottom; }), 1);
    EXPECT_GE(code_bg_edge([](const D2D1_RECT_F& r) { return r.top; }), 1);
}
