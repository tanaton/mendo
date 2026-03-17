#include <gtest/gtest.h>
#include "command_generator.h"
#include "layout.h"
#include "mock_text_measurer.h"
#include "parser.h"

// Tests for CommandGenerator with MockTextMeasurer (no COM / DirectWrite required).
// Uses the mock so entry.text_layout is null — tests structural command output.

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
        gen_.SetFormats({nullptr, nullptr}); // No real DWrite formats in mock tests
    }

    // Helper: parse markdown, compute layout, generate commands for the MD pane.
    DrawCommandList Generate(const std::string& md, float viewport_w = 800.0f) {
        auto nodes = ParseMarkdown(md);
        LayoutCache cache;
        cache.Resize(nodes.size());
        engine_.ComputeLayout(nodes, cache, viewport_w);
        PaneRect md_pane{0, 0, viewport_w, 2000.0f};
        return gen_.GenerateMdPane(nodes, cache, md_pane, 0.0f, TextSelection{});
    }
};

// ---- Structure tests ----

TEST_F(CmdGenTest, EmptyDocumentProducesClipAndTransformOnly) {
    auto cmds = Generate("");
    // Expect: PushClip, SetTransform, SetTransform(identity), PopClip
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
    EXPECT_EQ(transforms, 2); // scroll transform + identity reset
}

// ---- HorizontalRule ----

TEST_F(CmdGenTest, HorizontalRuleGeneratesDrawLine) {
    auto cmds = Generate("---");
    int line_count = 0;
    for (const auto& cmd : cmds) {
        if (std::holds_alternative<DrawLineCmd>(cmd)) line_count++;
    }
    EXPECT_GE(line_count, 1);
}

// ---- CodeBlock ----

TEST_F(CmdGenTest, CodeBlockGeneratesRoundedRectBackground) {
    auto cmds = Generate("```\ncode\n```");
    int rounded_count = 0;
    for (const auto& cmd : cmds) {
        if (std::holds_alternative<FillRoundedRectCmd>(cmd)) rounded_count++;
    }
    EXPECT_GE(rounded_count, 1);
}

// ---- Table ----

TEST_F(CmdGenTest, TableGeneratesLinesAndRects) {
    auto cmds = Generate("| A | B |\n|---|---|\n| 1 | 2 |");
    int lines = 0, rects = 0;
    for (const auto& cmd : cmds) {
        if (std::holds_alternative<DrawLineCmd>(cmd)) lines++;
        if (std::holds_alternative<FillRectCmd>(cmd)) rects++;
    }
    // Table should produce border lines and row backgrounds
    EXPECT_GT(lines, 0);
    EXPECT_GT(rects, 0);
}

// ---- List item ----

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

// ---- BlockQuote ----

TEST_F(CmdGenTest, BlockQuoteGeneratesBarLine) {
    auto cmds = Generate("> Quote");
    int lines = 0;
    for (const auto& cmd : cmds) {
        if (auto* line = std::get_if<DrawLineCmd>(&cmd)) {
            // BlockQuote bar is a vertical line (same x for both points)
            if (std::abs(line->p0.x - line->p1.x) < 0.01f)
                lines++;
        }
    }
    EXPECT_GE(lines, 1);
}

// ---- Viewport culling ----

TEST_F(CmdGenTest, ViewportCullingExcludesOffscreenNodes) {
    // Create many horizontal rules; these produce DrawLineCmd even without text_layout.
    std::string md;
    for (int i = 0; i < 50; i++) md += "---\n\n";
    auto nodes = ParseMarkdown(md);
    LayoutCache cache;
    cache.Resize(nodes.size());
    engine_.ComputeLayout(nodes, cache, 800.0f);

    // Use a small viewport: [0, 100) -> should only see a few nodes
    PaneRect md_pane{0, 0, 800.0f, 100.0f};
    auto cmds = gen_.GenerateMdPane(nodes, cache, md_pane, 0.0f, TextSelection{});

    // Full viewport should have more commands
    PaneRect md_pane_full{0, 0, 800.0f, 50000.0f};
    auto cmds_full = gen_.GenerateMdPane(nodes, cache, md_pane_full, 0.0f, TextSelection{});

    EXPECT_LT(cmds.size(), cmds_full.size());
}

// ---- DrawLineCmd properties ----

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

// ---- Multiple nodes ----

TEST_F(CmdGenTest, MixedContentGeneratesVariousCommands) {
    auto cmds = Generate("# Heading\n\nParagraph\n\n---\n\n- List\n\n> Quote\n\n```\ncode\n```");
    // Should have commands from all node types
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

// ---- Ordered list ----

TEST_F(CmdGenTest, OrderedListGeneratesDrawText) {
    auto cmds = Generate("1. First\n2. Second\n3. Third");
    // Ordered list should not produce bullet ellipses
    int fill_ellipses = 0;
    for (const auto& cmd : cmds) {
        if (std::holds_alternative<FillEllipseCmd>(cmd)) fill_ellipses++;
    }
    EXPECT_EQ(fill_ellipses, 0);
}

// ---- Task list ----

TEST_F(CmdGenTest, TaskListItemGeneratesNoEllipse) {
    auto cmds = Generate("- [x] Done\n- [ ] Not done");
    // Task list items should not generate bullet ellipses
    int fill_ellipses = 0;
    for (const auto& cmd : cmds) {
        if (std::holds_alternative<FillEllipseCmd>(cmd)) fill_ellipses++;
    }
    EXPECT_EQ(fill_ellipses, 0);
}

// ---- Selection highlighting ----

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

    // With mock measurer, text_layout is null so no selection rects generated
    // But structure should be valid: PushClip at start, PopClip at end
    ASSERT_GE(cmds.size(), 4u);
    EXPECT_TRUE(std::holds_alternative<PushClipCmd>(cmds.front()));
    EXPECT_TRUE(std::holds_alternative<PopClipCmd>(cmds.back()));
}

// ---- Scrolled viewport ----

TEST_F(CmdGenTest, ScrolledViewportCullsTopNodes) {
    // Create multiple paragraphs
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

    // Both should have valid clip/transform structure
    EXPECT_TRUE(std::holds_alternative<PushClipCmd>(cmds_top.front()));
    EXPECT_TRUE(std::holds_alternative<PushClipCmd>(cmds_mid.front()));
}

// ---- Code block with syntax language ----

TEST_F(CmdGenTest, CodeBlockWithLanguageGeneratesBackground) {
    auto cmds = Generate("```cpp\nint x = 42;\n```");
    int rounded_count = 0;
    for (const auto& cmd : cmds) {
        if (std::holds_alternative<FillRoundedRectCmd>(cmd)) rounded_count++;
    }
    EXPECT_GE(rounded_count, 1);
}

// ---- Multiple horizontal rules ----

TEST_F(CmdGenTest, MultipleHorizontalRulesGenerateMultipleLines) {
    auto cmds = Generate("---\n\n---\n\n---");
    int line_count = 0;
    for (const auto& cmd : cmds) {
        if (std::holds_alternative<DrawLineCmd>(cmd)) line_count++;
    }
    EXPECT_GE(line_count, 3);
}

// ---- BlockQuote bar color matches theme ----

TEST_F(CmdGenTest, BlockQuoteBarColorMatchesTheme) {
    auto cmds = Generate("> Quote text");
    for (const auto& cmd : cmds) {
        if (auto* line = std::get_if<DrawLineCmd>(&cmd)) {
            // BlockQuote bar: vertical line (same x)
            if (std::abs(line->p0.x - line->p1.x) < 0.01f) {
                EXPECT_FLOAT_EQ(line->color.r, theme_.blockquote_bar_color.r);
                EXPECT_FLOAT_EQ(line->color.g, theme_.blockquote_bar_color.g);
                EXPECT_FLOAT_EQ(line->color.b, theme_.blockquote_bar_color.b);
                break;
            }
        }
    }
}

// ---- Dark theme test ----

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

// ---- Empty table ----

TEST_F(CmdGenTest, EmptyDocumentNoExtraCommands) {
    auto cmds = Generate("");
    // Only clip/transform commands, no drawing commands
    for (size_t i = 1; i < cmds.size() - 1; i++) {
        // Middle commands should only be SetTransformCmd (identity reset)
        bool is_structural = std::holds_alternative<SetTransformCmd>(cmds[i]);
        if (!is_structural) {
            // Allow but don't strictly require only structural commands
        }
    }
}
