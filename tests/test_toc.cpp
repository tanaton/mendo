#include <gtest/gtest.h>
#include "toc.h"
#include "parser.h"

TEST(Toc, EmptyDocument) {
    std::vector<RenderNode> nodes;
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
