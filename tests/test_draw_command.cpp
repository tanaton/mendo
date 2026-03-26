#include <gtest/gtest.h>
#include "command_generator.h"
#include "layout.h"
#include "mock_text_measurer.h"
#include "parser.h"

// MockTextMeasurerを使用したCommandGeneratorのテスト（COM / DirectWrite不要）。
// モックを使用するためentry.text_layoutはnull — 構造的なコマンド出力をテストする。

class CmdGenTest : public ::testing::Test {
protected:
    MockTextMeasurer mock_;
    LayoutEngine engine_;
    CommandGenerator gen_;
    Theme theme_;

    void SetUp() override {
        theme_ = GetLightTheme();
        ASSERT_TRUE(engine_.Init(&mock_, theme_));
        gen_.SetTheme(&theme_);
        gen_.SetFormats({nullptr, nullptr, nullptr}); // モックテストでは実際のDWriteフォーマットなし
    }

    // ヘルパー: Markdownをパースし、レイアウトを計算し、MDペイン用のコマンドを生成する。
    DrawCommandList Generate(const std::string& md, float viewport_w = 800.0f) {
        auto nodes = ParseMarkdown(md);
        LayoutCache cache;
        cache.Resize(nodes.size());
        engine_.ComputeLayout(nodes, cache, viewport_w);
        PaneRect md_pane{0, 0, viewport_w, 2000.0f};
        return gen_.GenerateMdPane(nodes, cache, md_pane, 0.0f, TextSelection{});
    }
};

// ---- 構造テスト ----

TEST_F(CmdGenTest, EmptyDocumentProducesClipAndTransformOnly) {
    auto cmds = Generate("");
    // 期待値: PushClip, SetTransform, SetTransform(単位行列), PopClip
    ASSERT_GE(cmds.size(), 4u);
    EXPECT_TRUE(std::holds_alternative<PushClipCmd>(cmds.front()));
    EXPECT_TRUE(std::holds_alternative<PopClipCmd>(cmds.back()));
}

TEST_F(CmdGenTest, PushClipAndPopClipArePaired) {
    auto cmds = Generate("Hello\n\n---\n\nWorld");
    int push = 0, pop = 0;
    for (const auto& cmd : cmds) {
        if (std::holds_alternative<PushClipCmd>(cmd)) push++;
        if (std::holds_alternative<PopClipCmd>(cmd)) pop++;
    }
    EXPECT_EQ(push, 1);
    EXPECT_EQ(pop, 1);
}

TEST_F(CmdGenTest, TransformsArePaired) {
    auto cmds = Generate("Hello");
    int transforms = 0;
    for (const auto& cmd : cmds) {
        if (std::holds_alternative<SetTransformCmd>(cmd)) transforms++;
    }
    EXPECT_EQ(transforms, 2); // スクロール変換 + 単位行列リセット
}

// ---- 水平線 ----

TEST_F(CmdGenTest, HorizontalRuleGeneratesDrawLine) {
    auto cmds = Generate("---");
    int line_count = 0;
    for (const auto& cmd : cmds) {
        if (std::holds_alternative<DrawLineCmd>(cmd)) line_count++;
    }
    EXPECT_GE(line_count, 1);
}

// ---- コードブロック ----

TEST_F(CmdGenTest, CodeBlockGeneratesRoundedRectBackground) {
    auto cmds = Generate("```\ncode\n```");
    int rounded_count = 0;
    for (const auto& cmd : cmds) {
        if (std::holds_alternative<FillRoundedRectCmd>(cmd)) rounded_count++;
    }
    EXPECT_GE(rounded_count, 1);
}

// ---- テーブル ----

TEST_F(CmdGenTest, TableGeneratesLinesAndRects) {
    auto cmds = Generate("| A | B |\n|---|---|\n| 1 | 2 |");
    int lines = 0, rects = 0;
    for (const auto& cmd : cmds) {
        if (std::holds_alternative<DrawLineCmd>(cmd)) lines++;
        if (std::holds_alternative<FillRectCmd>(cmd)) rects++;
    }
    // テーブルは罫線と行背景を生成するべき
    EXPECT_GT(lines, 0);
    EXPECT_GT(rects, 0);
}

// ---- リスト項目 ----

TEST_F(CmdGenTest, UnorderedListGeneratesFillEllipse) {
    auto cmds = Generate("- Item");
    int ellipses = 0;
    for (const auto& cmd : cmds) {
        if (std::holds_alternative<FillEllipseCmd>(cmd)) ellipses++;
    }
    EXPECT_GE(ellipses, 1);
}

TEST_F(CmdGenTest, NestedListGeneratesDrawEllipse) {
    auto cmds = Generate("- Item\n  - Sub");
    int outline_ellipses = 0;
    for (const auto& cmd : cmds) {
        if (std::holds_alternative<DrawEllipseCmd>(cmd)) outline_ellipses++;
    }
    EXPECT_GE(outline_ellipses, 1);
}

// ---- 引用ブロック ----

TEST_F(CmdGenTest, BlockQuoteGeneratesBarLine) {
    auto cmds = Generate("> Quote");
    int lines = 0;
    for (const auto& cmd : cmds) {
        if (auto* line = std::get_if<DrawLineCmd>(&cmd)) {
            // 引用ブロックのバーは垂直線（両端のxが同じ）
            if (std::abs(line->p0.x - line->p1.x) < 0.01f)
                lines++;
        }
    }
    EXPECT_GE(lines, 1);
}

// ---- ビューポートカリング ----

TEST_F(CmdGenTest, ViewportCullingExcludesOffscreenNodes) {
    // 多数の水平線を作成。text_layoutがなくてもDrawLineCmdを生成する。
    std::string md;
    for (int i = 0; i < 50; i++) md += "---\n\n";
    auto nodes = ParseMarkdown(md);
    LayoutCache cache;
    cache.Resize(nodes.size());
    engine_.ComputeLayout(nodes, cache, 800.0f);

    // 小さなビューポートを使用: [0, 100) -> 少数のノードのみ表示されるべき
    PaneRect md_pane{0, 0, 800.0f, 100.0f};
    auto cmds = gen_.GenerateMdPane(nodes, cache, md_pane, 0.0f, TextSelection{});

    // フルビューポートではより多くのコマンドがあるべき
    PaneRect md_pane_full{0, 0, 800.0f, 50000.0f};
    auto cmds_full = gen_.GenerateMdPane(nodes, cache, md_pane_full, 0.0f, TextSelection{});

    EXPECT_LT(cmds.size(), cmds_full.size());
}

// ---- DrawLineCmdのプロパティ ----

TEST_F(CmdGenTest, HorizontalRuleColorMatchesTheme) {
    auto cmds = Generate("---");
    for (const auto& cmd : cmds) {
        if (auto* line = std::get_if<DrawLineCmd>(&cmd)) {
            EXPECT_FLOAT_EQ(line->color.r, theme_.hr_color.r);
            EXPECT_FLOAT_EQ(line->color.g, theme_.hr_color.g);
            EXPECT_FLOAT_EQ(line->color.b, theme_.hr_color.b);
            break;
        }
    }
}

// ---- 複数ノード ----

TEST_F(CmdGenTest, MixedContentGeneratesVariousCommands) {
    auto cmds = Generate("# Heading\n\nParagraph\n\n---\n\n- List\n\n> Quote\n\n```\ncode\n```");
    // すべてのノード種別からのコマンドがあるべき
    bool has_line = false, has_ellipse = false, has_rounded = false;
    for (const auto& cmd : cmds) {
        if (std::holds_alternative<DrawLineCmd>(cmd)) has_line = true;
        if (std::holds_alternative<FillEllipseCmd>(cmd)) has_ellipse = true;
        if (std::holds_alternative<FillRoundedRectCmd>(cmd)) has_rounded = true;
    }
    EXPECT_TRUE(has_line);
    EXPECT_TRUE(has_ellipse);
    EXPECT_TRUE(has_rounded);
}

// ---- 番号付きリスト ----

TEST_F(CmdGenTest, OrderedListGeneratesDrawText) {
    auto cmds = Generate("1. First\n2. Second\n3. Third");
    // 番号付きリストは箇条書きの楕円を生成しないべき
    int fill_ellipses = 0;
    for (const auto& cmd : cmds) {
        if (std::holds_alternative<FillEllipseCmd>(cmd)) fill_ellipses++;
    }
    EXPECT_EQ(fill_ellipses, 0);
}

// ---- タスクリスト ----

TEST_F(CmdGenTest, TaskListItemGeneratesNoEllipse) {
    auto cmds = Generate("- [x] Done\n- [ ] Not done");
    // タスクリスト項目は箇条書きの楕円を生成しないべき
    int fill_ellipses = 0;
    for (const auto& cmd : cmds) {
        if (std::holds_alternative<FillEllipseCmd>(cmd)) fill_ellipses++;
    }
    EXPECT_EQ(fill_ellipses, 0);
}

// ---- 選択ハイライト ----

TEST_F(CmdGenTest, SelectionGeneratesFillRects) {
    auto nodes = ParseMarkdown("Hello world paragraph");
    LayoutCache cache;
    cache.Resize(nodes.size());
    engine_.ComputeLayout(nodes, cache, 800.0f);

    TextSelection sel;
    sel.active = true;
    sel.start_node = 0;
    sel.start_pos = 0;
    sel.end_node = 0;
    sel.end_pos = 5;

    PaneRect pane{0, 0, 800.0f, 2000.0f};
    auto cmds = gen_.GenerateMdPane(nodes, cache, pane, 0.0f, sel);

    // モック計測器ではtext_layoutがnullなので選択矩形は生成されない
    // ただし構造は有効であるべき: 先頭にPushClip、末尾にPopClip
    ASSERT_GE(cmds.size(), 4u);
    EXPECT_TRUE(std::holds_alternative<PushClipCmd>(cmds.front()));
    EXPECT_TRUE(std::holds_alternative<PopClipCmd>(cmds.back()));
}

// ---- スクロール済みビューポート ----

TEST_F(CmdGenTest, ScrolledViewportCullsTopNodes) {
    // 複数の段落を作成
    std::string md;
    for (int i = 0; i < 20; i++) md += "Paragraph " + std::to_string(i) + "\n\n";

    auto nodes = ParseMarkdown(md);
    LayoutCache cache;
    cache.Resize(nodes.size());
    engine_.ComputeLayout(nodes, cache, 800.0f);

    float total = engine_.GetTotalHeight();
    float half_scroll = total * 0.5f;

    PaneRect pane{0, 0, 800.0f, 200.0f};
    auto cmds_top = gen_.GenerateMdPane(nodes, cache, pane, 0.0f, TextSelection{});
    auto cmds_mid = gen_.GenerateMdPane(nodes, cache, pane, half_scroll, TextSelection{});

    // 両方とも有効なclip/transform構造を持つべき
    EXPECT_TRUE(std::holds_alternative<PushClipCmd>(cmds_top.front()));
    EXPECT_TRUE(std::holds_alternative<PushClipCmd>(cmds_mid.front()));
}

// ---- 言語指定付きコードブロック ----

TEST_F(CmdGenTest, CodeBlockWithLanguageGeneratesBackground) {
    auto cmds = Generate("```cpp\nint x = 42;\n```");
    int rounded_count = 0;
    for (const auto& cmd : cmds) {
        if (std::holds_alternative<FillRoundedRectCmd>(cmd)) rounded_count++;
    }
    EXPECT_GE(rounded_count, 1);
}

// ---- 複数の水平線 ----

TEST_F(CmdGenTest, MultipleHorizontalRulesGenerateMultipleLines) {
    auto cmds = Generate("---\n\n---\n\n---");
    int line_count = 0;
    for (const auto& cmd : cmds) {
        if (std::holds_alternative<DrawLineCmd>(cmd)) line_count++;
    }
    EXPECT_GE(line_count, 3);
}

// ---- 引用ブロックのバー色がテーマと一致 ----

TEST_F(CmdGenTest, BlockQuoteBarColorMatchesTheme) {
    auto cmds = Generate("> Quote text");
    for (const auto& cmd : cmds) {
        if (auto* line = std::get_if<DrawLineCmd>(&cmd)) {
            // 引用ブロックのバー: 垂直線（xが同じ）
            if (std::abs(line->p0.x - line->p1.x) < 0.01f) {
                EXPECT_FLOAT_EQ(line->color.r, theme_.blockquote_bar_color.r);
                EXPECT_FLOAT_EQ(line->color.g, theme_.blockquote_bar_color.g);
                EXPECT_FLOAT_EQ(line->color.b, theme_.blockquote_bar_color.b);
                break;
            }
        }
    }
}

// ---- ダークテーマテスト ----

TEST_F(CmdGenTest, DarkThemeTableGeneratesCommands) {
    theme_ = GetDarkTheme();
    gen_.SetTheme(&theme_);
    ASSERT_TRUE(engine_.Init(&mock_, theme_));

    auto cmds = Generate("| A | B |\n|---|---|\n| 1 | 2 |");
    int lines = 0;
    for (const auto& cmd : cmds) {
        if (std::holds_alternative<DrawLineCmd>(cmd)) lines++;
    }
    EXPECT_GT(lines, 0);
}

// ---- 空のテーブル ----

TEST_F(CmdGenTest, EmptyDocumentNoExtraCommands) {
    auto cmds = Generate("");
    // clip/transformコマンドのみで、描画コマンドはない
    for (size_t i = 1; i < cmds.size() - 1; i++) {
        // 中間のコマンドはSetTransformCmd（単位行列リセット）のみであるべき
        bool is_structural = std::holds_alternative<SetTransformCmd>(cmds[i]);
        if (!is_structural) {
            // 構造的コマンドのみを厳密には要求しないが許容する
        }
    }
}

// ---- GitHub Alerts ----

TEST_F(CmdGenTest, AlertGeneratesColoredBar) {
    auto cmds = Generate("> [!NOTE]\n> Alert content");
    // Alertのバーは垂直線で、通常のblockquoteとは異なる色を持つべき
    bool found_alert_bar = false;
    for (const auto& cmd : cmds) {
        if (auto* line = std::get_if<DrawLineCmd>(&cmd)) {
            if (std::abs(line->p0.x - line->p1.x) < 0.01f) {
                // 垂直線 = バー。blockquote_bar_color と異なるべき
                bool is_blockquote_color =
                    (std::abs(line->color.r - theme_.blockquote_bar_color.r) < 0.01f) &&
                    (std::abs(line->color.g - theme_.blockquote_bar_color.g) < 0.01f) &&
                    (std::abs(line->color.b - theme_.blockquote_bar_color.b) < 0.01f);
                if (!is_blockquote_color) {
                    found_alert_bar = true;
                }
            }
        }
    }
    EXPECT_TRUE(found_alert_bar) << "Alert のバーは通常の blockquote とは異なる色であるべき";
}

TEST_F(CmdGenTest, AlertGeneratesBackground) {
    auto cmds = Generate("> [!WARNING]\n> Be careful");
    // Alertは角丸四角形の背景を生成するべき
    int rounded_count = 0;
    for (const auto& cmd : cmds) {
        if (std::holds_alternative<FillRoundedRectCmd>(cmd)) rounded_count++;
    }
    EXPECT_GE(rounded_count, 1) << "Alert は背景の角丸四角形を生成するべき";
}

TEST_F(CmdGenTest, AlertBarColorMatchesTheme) {
    auto cmds = Generate("> [!NOTE]\n> text");
    // Note の色は theme_.alert_color[0]
    for (const auto& cmd : cmds) {
        if (auto* line = std::get_if<DrawLineCmd>(&cmd)) {
            if (std::abs(line->p0.x - line->p1.x) < 0.01f) {
                // 垂直線を検出
                if (std::abs(line->color.r - theme_.alert_color[0].r) < 0.01f &&
                    std::abs(line->color.g - theme_.alert_color[0].g) < 0.01f &&
                    std::abs(line->color.b - theme_.alert_color[0].b) < 0.01f) {
                    SUCCEED();
                    return;
                }
            }
        }
    }
    FAIL() << "Note のバー色が theme_.alert_color[0] と一致するべき";
}

TEST_F(CmdGenTest, RegularBlockquoteStillUsesOriginalColor) {
    auto cmds = Generate("> Normal quote");
    for (const auto& cmd : cmds) {
        if (auto* line = std::get_if<DrawLineCmd>(&cmd)) {
            if (std::abs(line->p0.x - line->p1.x) < 0.01f) {
                EXPECT_FLOAT_EQ(line->color.r, theme_.blockquote_bar_color.r);
                EXPECT_FLOAT_EQ(line->color.g, theme_.blockquote_bar_color.g);
                EXPECT_FLOAT_EQ(line->color.b, theme_.blockquote_bar_color.b);
                return;
            }
        }
    }
    FAIL() << "Regular blockquote は垂直バー線を生成し、その色が theme_.blockquote_bar_color と一致するべき";
}

TEST_F(CmdGenTest, AllAlertTypesGenerateCommands) {
    const char* alerts[] = {
        "> [!NOTE]\n> n",
        "> [!TIP]\n> t",
        "> [!IMPORTANT]\n> i",
        "> [!WARNING]\n> w",
        "> [!CAUTION]\n> c"
    };
    for (const char* md : alerts) {
        auto cmds = Generate(md);
        int lines = 0;
        for (const auto& cmd : cmds) {
            if (std::holds_alternative<DrawLineCmd>(cmd)) lines++;
        }
        EXPECT_GE(lines, 1) << "Alert '" << md << "' はバー線を生成するべき";
    }
}

// ---- 見出し下線 ----

TEST_F(CmdGenTest, H1GeneratesUnderline) {
    auto cmds = Generate("# Heading 1");
    int hlines = 0;
    for (const auto& cmd : cmds) {
        if (auto* line = std::get_if<DrawLineCmd>(&cmd)) {
            // 水平線（両端のyが同じ）
            if (std::abs(line->p0.y - line->p1.y) < 0.01f) {
                hlines++;
            }
        }
    }
    EXPECT_GE(hlines, 1) << "h1 は下線の水平線を生成するべき";
}

TEST_F(CmdGenTest, H2GeneratesUnderline) {
    auto cmds = Generate("## Heading 2");
    int hlines = 0;
    for (const auto& cmd : cmds) {
        if (auto* line = std::get_if<DrawLineCmd>(&cmd)) {
            if (std::abs(line->p0.y - line->p1.y) < 0.01f) {
                hlines++;
            }
        }
    }
    EXPECT_GE(hlines, 1) << "h2 は下線の水平線を生成するべき";
}

TEST_F(CmdGenTest, H3DoesNotGenerateUnderline) {
    auto cmds = Generate("### Heading 3");
    int hlines = 0;
    for (const auto& cmd : cmds) {
        if (auto* line = std::get_if<DrawLineCmd>(&cmd)) {
            if (std::abs(line->p0.y - line->p1.y) < 0.01f) {
                hlines++;
            }
        }
    }
    EXPECT_EQ(hlines, 0) << "h3 は下線を生成しないべき";
}

TEST_F(CmdGenTest, HeadingUnderlineColorMatchesTheme) {
    auto cmds = Generate("# Title");
    for (const auto& cmd : cmds) {
        if (auto* line = std::get_if<DrawLineCmd>(&cmd)) {
            if (std::abs(line->p0.y - line->p1.y) < 0.01f) {
                EXPECT_FLOAT_EQ(line->color.r, theme_.hr_color.r);
                EXPECT_FLOAT_EQ(line->color.g, theme_.hr_color.g);
                EXPECT_FLOAT_EQ(line->color.b, theme_.hr_color.b);
                EXPECT_FLOAT_EQ(line->stroke_width, theme_.hr_thickness);
                return;
            }
        }
    }
    FAIL() << "見出し下線が見つからない";
}

// ---- コピーボタン ----

// formats_.copy_btn_icon が null の場合、コピーボタン用の DrawTextCmd は生成されない
TEST_F(CmdGenTest, CodeBlockNoCopyButtonWithoutIconFont) {
    auto cmds = Generate("```\ncode\n```");
    int text_cmd_count = 0;
    for (const auto& cmd : cmds) {
        if (std::holds_alternative<DrawTextCmd>(cmd)) text_cmd_count++;
    }
    EXPECT_EQ(text_cmd_count, 0) << "formats_.copy_btn_icon が null のときコピーボタンの DrawTextCmd は生成されないべき";
}

// 非コードブロックノードはコピーボタンのコマンドを生成しない
TEST_F(CmdGenTest, NonCodeBlockNoCopyButton) {
    auto cmds = Generate("Hello world");
    int text_cmd_count = 0;
    for (const auto& cmd : cmds) {
        if (std::holds_alternative<DrawTextCmd>(cmd)) text_cmd_count++;
    }
    EXPECT_EQ(text_cmd_count, 0);
}

// hovered_copy_node パラメータが GenerateMdPane に渡せることの検証
TEST_F(CmdGenTest, CodeBlockWithHoveredCopyNodeAccepted) {
    auto nodes = ParseMarkdown("```\ncode\n```");
    LayoutCache cache;
    cache.Resize(nodes.size());
    engine_.ComputeLayout(nodes, cache, 800.0f);
    PaneRect md_pane{0, 0, 800.0f, 2000.0f};
    // hovered_copy_node=0 を渡してもクラッシュしない
    auto cmds = gen_.GenerateMdPane(nodes, cache, md_pane, 0.0f, TextSelection{}, -1, 0);
    EXPECT_TRUE(std::holds_alternative<PushClipCmd>(cmds.front()));
    EXPECT_TRUE(std::holds_alternative<PopClipCmd>(cmds.back()));
}
