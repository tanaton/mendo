#include <gtest/gtest.h>
#include "cmd_gen_mock_test_base.h"
#include "test_helpers.h"
#include <cmath>
#include <optional>

// MockTextMeasurerを使用したCommandGeneratorのテスト（COM / DirectWrite不要）。
// モックを使用するためentry.text_layoutはnull — 構造的なコマンド出力をテストする。

namespace {

class CmdGenTest : public CmdGenMockTestBase {
protected:
    // 呼び出し後も nodes_ / cache_ で期待値を組み立てられる。
    DrawCommandList Generate(std::string_view md, float viewport_w = 800.0f)
    {
        Parse(md, viewport_w);
        const PaneRect md_pane{ 0, 0, viewport_w, 2000.0f };
        return gen_.GenerateMdPane(nodes_, cache_, md_pane, 0.0f, TextSelection{});
    }
};

bool IsVertical(const DrawLineCmd& l)
{
    return std::abs(l.p0.x - l.p1.x) < 0.01f;
}

bool IsHorizontal(const DrawLineCmd& l)
{
    return std::abs(l.p0.y - l.p1.y) < 0.01f;
}

std::ptrdiff_t CountVerticalLines(const DrawCommandList& cmds)
{
    return std::ranges::count_if(cmds, [](const DrawCommand& c) {
        const auto* l = std::get_if<DrawLineCmd>(&c);
        return l && IsVertical(*l);
    });
}

} // namespace

// ---- 構造テスト ----

TEST_F(CmdGenTest, PushClipAndPopClipArePaired)
{
    auto cmds = Generate("Hello\n\n---\n\nWorld");
    EXPECT_EQ(CountCmd<PushClipCmd>(cmds), 1);
    EXPECT_EQ(CountCmd<PopClipCmd>(cmds), 1);
}

TEST_F(CmdGenTest, TransformsArePaired)
{
    auto cmds = Generate("Hello");
    EXPECT_EQ(CountCmd<SetTransformCmd>(cmds), 2); // スクロール変換 + 単位行列リセット
}

// ---- コードブロック ----

TEST_F(CmdGenTest, CodeBlockGeneratesRoundedRectBackground)
{
    auto cmds = Generate("```\ncode\n```");
    EXPECT_GE(CountCmd<FillRoundedRectCmd>(cmds), 1);
}

// ---- テーブル ----

TEST_F(CmdGenTest, TableGeneratesLinesAndRects)
{
    auto cmds = Generate("| A | B |\n|---|---|\n| 1 | 2 |");
    // テーブルは罫線と行背景を生成するべき
    EXPECT_GT(CountCmd<DrawLineCmd>(cmds), 0);
    EXPECT_GT(CountCmd<FillRectCmd>(cmds), 0);
}

// ---- リスト項目 ----

TEST_F(CmdGenTest, UnorderedListGeneratesFillEllipse)
{
    auto cmds = Generate("- Item");
    EXPECT_GE(CountCmd<FillEllipseCmd>(cmds), 1);
}

TEST_F(CmdGenTest, NestedListGeneratesDrawEllipse)
{
    auto cmds = Generate("- Item\n  - Sub");
    EXPECT_GE(CountCmd<DrawEllipseCmd>(cmds), 1);
}

// ---- 引用ブロック ----

TEST_F(CmdGenTest, MultiLineBlockQuoteGeneratesSingleBar)
{
    auto cmds = Generate("> Line 1\n>\n> Line 2");
    EXPECT_EQ(CountVerticalLines(cmds), 1) << "複数行の引用ブロックは1本の統合されたバーのみ生成するべき";
}

TEST_F(CmdGenTest, MultiLineAlertGeneratesSingleBarAndBackground)
{
    auto cmds = Generate("> [!NOTE]\n> Line 1\n>\n> Line 2");
    EXPECT_EQ(CountVerticalLines(cmds), 1) << "複数行のAlertは1本の統合されたバーのみ生成するべき";
    EXPECT_EQ(CountCmd<FillRoundedRectCmd>(cmds), 1) << "複数行のAlertは1つの統合された背景のみ生成するべき";
}

// ---- ビューポートカリング ----

TEST_F(CmdGenTest, ViewportCullingExcludesOffscreenNodes)
{
    // 多数の水平線を作成。text_layoutがなくてもDrawLineCmdを生成する。
    std::string md;
    for (int i = 0; i < 50; i++) {
        md += "---\n\n";
    }
    Parse(md);

    // 小さなビューポートを使用: [0, 100) -> 少数のノードのみ表示されるべき
    const PaneRect md_pane{ 0, 0, 800.0f, 100.0f };
    const size_t culled = gen_.GenerateMdPane(nodes_, cache_, md_pane, 0.0f, TextSelection{}).size();

    // フルビューポートではより多くのコマンドがあるべき
    const PaneRect md_pane_full{ 0, 0, 800.0f, 50000.0f };
    const size_t full = gen_.GenerateMdPane(nodes_, cache_, md_pane_full, 0.0f, TextSelection{}).size();

    EXPECT_LT(culled, full);
}

// ---- DrawLineCmdのプロパティ ----

TEST_F(CmdGenTest, HorizontalRuleColorMatchesTheme)
{
    auto cmds = Generate("---");
    const auto line = FindFirst<DrawLineCmd>(cmds);
    ASSERT_TRUE(line.has_value());
    EXPECT_FLOAT_EQ(line->color.r, theme_.hr_color.r);
    EXPECT_FLOAT_EQ(line->color.g, theme_.hr_color.g);
    EXPECT_FLOAT_EQ(line->color.b, theme_.hr_color.b);
}

// ---- 複数ノード ----

TEST_F(CmdGenTest, MixedContentGeneratesVariousCommands)
{
    auto cmds = Generate("# Heading\n\nParagraph\n\n---\n\n- List\n\n> Quote\n\n```\ncode\n```");
    // すべてのノード種別からのコマンドがあるべき
    EXPECT_GT(CountCmd<DrawLineCmd>(cmds), 0);
    EXPECT_GT(CountCmd<FillEllipseCmd>(cmds), 0);
    EXPECT_GT(CountCmd<FillRoundedRectCmd>(cmds), 0);
}

// ---- 番号付きリスト ----

TEST_F(CmdGenTest, OrderedListGeneratesDrawText)
{
    auto cmds = Generate("1. First\n2. Second\n3. Third");
    // 番号付きリストは箇条書きの楕円を生成しないべき
    EXPECT_EQ(CountCmd<FillEllipseCmd>(cmds), 0);
}

// ---- タスクリスト ----

TEST_F(CmdGenTest, TaskListItemGeneratesNoEllipse)
{
    auto cmds = Generate("- [x] Done\n- [ ] Not done");
    // タスクリスト項目は箇条書きの楕円を生成しないべき
    EXPECT_EQ(CountCmd<FillEllipseCmd>(cmds), 0);
}

// ---- 言語指定付きコードブロック ----

TEST_F(CmdGenTest, CodeBlockWithLanguageGeneratesBackground)
{
    auto cmds = Generate("```cpp\nint x = 42;\n```");
    EXPECT_GE(CountCmd<FillRoundedRectCmd>(cmds), 1);
}

// ---- 複数の水平線 ----

TEST_F(CmdGenTest, MultipleHorizontalRulesGenerateMultipleLines)
{
    auto cmds = Generate("---\n\n---\n\n---");
    EXPECT_GE(CountCmd<DrawLineCmd>(cmds), 3);
}

// ---- ダークテーマテスト ----

TEST_F(CmdGenTest, DarkThemeTableGeneratesCommands)
{
    theme_ = GetDarkTheme();
    gen_.SetTheme(&theme_);
    ASSERT_TRUE(engine_.Init(&mock_, theme_));

    auto cmds = Generate("| A | B |\n|---|---|\n| 1 | 2 |");
    EXPECT_GT(CountCmd<DrawLineCmd>(cmds), 0);
}

// ---- GitHub Alerts ----

TEST_F(CmdGenTest, AlertGeneratesBackground)
{
    auto cmds = Generate("> [!WARNING]\n> Be careful");
    EXPECT_GE(CountCmd<FillRoundedRectCmd>(cmds), 1) << "Alert は背景の角丸四角形を生成するべき";
}

TEST_F(CmdGenTest, AlertBarColorMatchesTheme)
{
    // Note の色は theme_.alert_color[0]。通常の blockquote のバーとは異なる色であるべき。
    ASSERT_FALSE(ColorEq(theme_.alert_color[0], theme_.blockquote_bar_color));
    auto cmds = Generate("> [!NOTE]\n> text");
    const auto bar = FindFirst<DrawLineCmd>(cmds, [&](const DrawLineCmd& l) {
        return IsVertical(l) && ColorEq(l.color, theme_.alert_color[0]);
    });
    EXPECT_TRUE(bar.has_value()) << "Note のバー色が theme_.alert_color[0] と一致するべき";
}

TEST_F(CmdGenTest, RegularBlockquoteStillUsesOriginalColor)
{
    auto cmds = Generate("> Normal quote");
    const auto bar = FindFirst<DrawLineCmd>(cmds, IsVertical);
    ASSERT_TRUE(bar.has_value()) << "Regular blockquote は垂直バー線を生成するべき";
    EXPECT_FLOAT_EQ(bar->color.r, theme_.blockquote_bar_color.r);
    EXPECT_FLOAT_EQ(bar->color.g, theme_.blockquote_bar_color.g);
    EXPECT_FLOAT_EQ(bar->color.b, theme_.blockquote_bar_color.b);
}

TEST_F(CmdGenTest, AllAlertTypesGenerateCommands)
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
        auto cmds = Generate(a.md);
        EXPECT_GE(CountCmd<DrawLineCmd>(cmds), 1) << "Alert '" << a.name << "' はバー線を生成するべき";
    }
}

// ---- 見出し下線 ----

TEST_F(CmdGenTest, HeadingUnderlineColorMatchesTheme)
{
    auto cmds = Generate("# Title");
    const auto underline = FindFirst<DrawLineCmd>(cmds, IsHorizontal);
    ASSERT_TRUE(underline.has_value()) << "見出し下線が見つからない";
    EXPECT_FLOAT_EQ(underline->color.r, theme_.hr_color.r);
    EXPECT_FLOAT_EQ(underline->color.g, theme_.hr_color.g);
    EXPECT_FLOAT_EQ(underline->color.b, theme_.hr_color.b);
    EXPECT_FLOAT_EQ(underline->stroke_width, theme_.hr_thickness);
}

TEST_F(CmdGenTest, H2UnderlineThicknessMatchesTheme)
{
    auto cmds = Generate("## Heading 2");
    const auto underline = FindFirst<DrawLineCmd>(cmds, IsHorizontal);
    ASSERT_TRUE(underline.has_value()) << "h2 の見出し下線が見つからない";
    EXPECT_FLOAT_EQ(underline->stroke_width, theme_.h2_underline_thickness);
    EXPECT_LT(underline->stroke_width, theme_.hr_thickness);
}

// ---- コピーボタン ----

// formats_.copy_btn_icon が null の場合、コピーボタン用の DrawTextCmd は生成されない
TEST_F(CmdGenTest, CodeBlockNoCopyButtonWithoutIconFont)
{
    auto cmds = Generate("```\ncode\n```");
    EXPECT_EQ(CountCmd<DrawTextCmd>(cmds), 0) << "formats_.copy_btn_icon が null のときコピーボタンの DrawTextCmd は生成されないべき";
}

// 非コードブロックノードはコピーボタンのコマンドを生成しない
TEST_F(CmdGenTest, NonCodeBlockNoCopyButton)
{
    auto cmds = Generate("Hello world");
    EXPECT_EQ(CountCmd<DrawTextCmd>(cmds), 0);
}

// ---- リスト箇条書き記号の垂直位置 ----

TEST_F(CmdGenTest, UnorderedListBulletCenteredOnFirstLine)
{
    auto cmds = Generate("- Item");

    // text_layout が null のフォールバック: first_line_h = font_size_body * 1.3
    // bullet 中心は物理ピクセル境界へスナップされるため、期待値も同じ規則でスナップする。
    const float expected_y = SnapToPhysicalPixel(cache_.Top(0) + theme_.font_size_body * 1.3f * 0.5f, 1.0f);

    const auto bullet = FindFirst<FillEllipseCmd>(cmds);
    ASSERT_TRUE(bullet.has_value()) << "FillEllipseCmd が見つからない";
    EXPECT_NEAR(bullet->center.y, expected_y, 0.01f);
}

TEST_F(CmdGenTest, NestedListBulletCenteredOnFirstLine)
{
    auto cmds = Generate("- A\n  - B");

    // bullet 中心は物理ピクセル境界へスナップされるため、期待値も同じ規則でスナップする。
    const float expected_y0 = SnapToPhysicalPixel(cache_.Top(0) + theme_.font_size_body * 1.3f * 0.5f, 1.0f);
    const float expected_y1 = SnapToPhysicalPixel(cache_.Top(1) + theme_.font_size_body * 1.3f * 0.5f, 1.0f);

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

// HoveredButtons パラメータが GenerateMdPane に渡せることの検証
TEST_F(CmdGenTest, CodeBlockWithHoveredCopyNodeAccepted)
{
    Parse("```\ncode\n```");
    const PaneRect md_pane{ 0, 0, 800.0f, 2000.0f };
    // hovered.copy=0 を渡してもクラッシュしない
    const auto& cmds = gen_.GenerateMdPane(nodes_, cache_, md_pane, 0.0f, TextSelection{}, -1, HoveredButtons{ 0, -1, -1 });
    EXPECT_TRUE(std::holds_alternative<PushClipCmd>(cmds.front()));
    EXPECT_TRUE(std::holds_alternative<PopClipCmd>(cmds.back()));
}

// ---- 図・画像プレースホルダー ----

TEST_F(CmdGenTest, ImageWithoutBitmapGeneratesPlaceholder)
{
    // bitmapを設定しない → プレースホルダーが描画されるべき
    auto cmds = Generate("![alt](image.png)");
    EXPECT_GE(CountCmd<FillRoundedRectCmd>(cmds), 1) << "ビットマップ未設定の画像はプレースホルダー背景を描画するべき";
}

TEST_F(CmdGenTest, MermaidWithoutBitmapGeneratesPlaceholder)
{
    // bitmapを設定しない → プレースホルダーが描画されるべき
    auto cmds = Generate("```mermaid\ngraph TD\n  A-->B\n```");
    EXPECT_GE(CountCmd<FillRoundedRectCmd>(cmds), 1) << "ビットマップ未設定のMermaidはプレースホルダー背景を描画するべき";
}

TEST_F(CmdGenTest, PlaceholderBgUsesCodeBgColor)
{
    auto cmds = Generate("![alt](image.png)");
    const auto rr = FindFirst<FillRoundedRectCmd>(cmds);
    ASSERT_TRUE(rr.has_value()) << "プレースホルダーの角丸四角形が見つからない";
    EXPECT_FLOAT_EQ(rr->color.r, theme_.code_bg_color.r);
    EXPECT_FLOAT_EQ(rr->color.g, theme_.code_bg_color.g);
    EXPECT_FLOAT_EQ(rr->color.b, theme_.code_bg_color.b);
}
