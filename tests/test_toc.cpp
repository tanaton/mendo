#include <gtest/gtest.h>
#include "toc.h"
#include "parser.h"

TEST(Toc, EmptyDocument) {
    std::vector<Node> nodes;
    TableOfContents toc;
    toc.BuildFromNodes(nodes);
    EXPECT_TRUE(toc.GetEntries().empty());
}

TEST(Toc, NoHeadings) {
    auto nodes = ParseMarkdown("Just a paragraph.\n\n- List item\n\n> Quote");
    TableOfContents toc;
    toc.BuildFromNodes(nodes);
    EXPECT_TRUE(toc.GetEntries().empty());
}

TEST(Toc, SingleHeading) {
    auto nodes = ParseMarkdown("# Title");
    TableOfContents toc;
    toc.BuildFromNodes(nodes);
    ASSERT_EQ(toc.GetEntries().size(), 1u);
    EXPECT_EQ(toc.GetEntries()[0].text, L"Title");
    EXPECT_EQ(toc.GetEntries()[0].heading_level, 1);
    EXPECT_EQ(toc.GetEntries()[0].anchor_id, L"title");
}

TEST(Toc, MultipleHeadings) {
    auto nodes = ParseMarkdown(
        "# First\n\n## Second\n\n### Third\n\nParagraph\n\n## Another"
    );
    TableOfContents toc;
    toc.BuildFromNodes(nodes);
    ASSERT_EQ(toc.GetEntries().size(), 4u);
    EXPECT_EQ(toc.GetEntries()[0].heading_level, 1);
    EXPECT_EQ(toc.GetEntries()[1].heading_level, 2);
    EXPECT_EQ(toc.GetEntries()[2].heading_level, 3);
    EXPECT_EQ(toc.GetEntries()[3].heading_level, 2);
}

TEST(Toc, HeadingTextPreserved) {
    auto nodes = ParseMarkdown("## Hello World");
    TableOfContents toc;
    toc.BuildFromNodes(nodes);
    ASSERT_EQ(toc.GetEntries().size(), 1u);
    EXPECT_EQ(toc.GetEntries()[0].text, L"Hello World");
}

TEST(Toc, AnchorIdPreserved) {
    auto nodes = ParseMarkdown("## コードブロック");
    TableOfContents toc;
    toc.BuildFromNodes(nodes);
    ASSERT_EQ(toc.GetEntries().size(), 1u);
    EXPECT_EQ(toc.GetEntries()[0].anchor_id, L"コードブロック");
}

TEST(Toc, RebuildClearsPrevious) {
    auto nodes1 = ParseMarkdown("# A\n\n## B");
    auto nodes2 = ParseMarkdown("# X");

    TableOfContents toc;
    toc.BuildFromNodes(nodes1);
    EXPECT_EQ(toc.GetEntries().size(), 2u);

    toc.BuildFromNodes(nodes2);
    EXPECT_EQ(toc.GetEntries().size(), 1u);
    EXPECT_EQ(toc.GetEntries()[0].text, L"X");
}

// ---- HitTest ----

TEST(Toc, HitTestValidIndex) {
    auto nodes = ParseMarkdown("# A\n\n## B\n\n### C");
    TableOfContents toc;
    toc.BuildFromNodes(nodes);

    EXPECT_EQ(toc.HitTest(0.0f, 28.0f), 0);
    EXPECT_EQ(toc.HitTest(28.0f, 28.0f), 1);
    EXPECT_EQ(toc.HitTest(56.0f, 28.0f), 2);
}

TEST(Toc, HitTestOutOfRange) {
    auto nodes = ParseMarkdown("# A");
    TableOfContents toc;
    toc.BuildFromNodes(nodes);

    EXPECT_EQ(toc.HitTest(-1.0f, 28.0f), -1);
    EXPECT_EQ(toc.HitTest(100.0f, 28.0f), -1);
}

TEST(Toc, HitTestZeroItemHeight) {
    auto nodes = ParseMarkdown("# A");
    TableOfContents toc;
    toc.BuildFromNodes(nodes);
    EXPECT_EQ(toc.HitTest(10.0f, 0.0f), -1);
}

TEST(Toc, HitTestEmpty) {
    TableOfContents toc;
    EXPECT_EQ(toc.HitTest(0.0f, 28.0f), -1);
}

// ---- Additional edge cases ----

TEST(Toc, DuplicateHeadingText) {
    auto nodes = ParseMarkdown("# Title\n\nSome text\n\n# Title\n\nMore text\n\n## Title");
    TableOfContents toc;
    toc.BuildFromNodes(nodes);
    ASSERT_EQ(toc.GetEntries().size(), 3u);
    // All should have same text
    for (const auto& entry : toc.GetEntries()) {
        EXPECT_EQ(entry.text, L"Title");
    }
    // But anchor_ids should be unique (after parser refactoring)
    EXPECT_NE(toc.GetEntries()[0].anchor_id, toc.GetEntries()[1].anchor_id);
}

TEST(Toc, ManyHeadings) {
    std::string md;
    for (int i = 0; i < 100; i++) {
        md += "## Heading " + std::to_string(i) + "\n\ntext\n\n";
    }
    auto nodes = ParseMarkdown(md);
    TableOfContents toc;
    toc.BuildFromNodes(nodes);
    EXPECT_EQ(toc.GetEntries().size(), 100u);
}

TEST(Toc, HeadingLevelsPreserved) {
    auto nodes = ParseMarkdown(
        "# L1\n\n## L2\n\n### L3\n\n#### L4\n\n##### L5\n\n###### L6"
    );
    TableOfContents toc;
    toc.BuildFromNodes(nodes);
    ASSERT_EQ(toc.GetEntries().size(), 6u);
    for (int i = 0; i < 6; i++) {
        EXPECT_EQ(toc.GetEntries()[i].heading_level, i + 1);
    }
}

TEST(Toc, HitTestBoundary) {
    auto nodes = ParseMarkdown("# A\n\n## B");
    TableOfContents toc;
    toc.BuildFromNodes(nodes);
    // Exactly at boundary between items
    EXPECT_EQ(toc.HitTest(27.9f, 28.0f), 0);
    EXPECT_EQ(toc.HitTest(28.0f, 28.0f), 1);
}

TEST(Toc, HitTestNegativeItemHeight) {
    auto nodes = ParseMarkdown("# A");
    TableOfContents toc;
    toc.BuildFromNodes(nodes);
    EXPECT_EQ(toc.HitTest(10.0f, -1.0f), -1);
}
