#include <gtest/gtest.h>
#include <memory_resource>
#include "toc.h"
#include "parser.h"

TEST(Toc, EmptyDocument)
{
    std::pmr::vector<Node> nodes;
    TableOfContents toc;
    toc.BuildFromNodes(nodes);
    EXPECT_TRUE(toc.GetEntries().empty());
}

TEST(Toc, NoHeadings)
{
    auto nodes = ParseMarkdown("Just a paragraph.\n\n- List item\n\n> Quote");
    TableOfContents toc;
    toc.BuildFromNodes(nodes);
    EXPECT_TRUE(toc.GetEntries().empty());
}

TEST(Toc, SingleHeading)
{
    auto nodes = ParseMarkdown("# Title");
    TableOfContents toc;
    toc.BuildFromNodes(nodes);
    ASSERT_EQ(toc.GetEntries().size(), 1u);
    EXPECT_EQ(toc.GetEntries()[0].text, L"Title");
    EXPECT_EQ(toc.GetEntries()[0].heading_level, 1);
    EXPECT_EQ(toc.GetEntries()[0].anchor_id, L"title");
}

TEST(Toc, MultipleHeadings)
{
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

TEST(Toc, HeadingTextPreserved)
{
    auto nodes = ParseMarkdown("## Hello World");
    TableOfContents toc;
    toc.BuildFromNodes(nodes);
    ASSERT_EQ(toc.GetEntries().size(), 1u);
    EXPECT_EQ(toc.GetEntries()[0].text, L"Hello World");
}

TEST(Toc, AnchorIdPreserved)
{
    auto nodes = ParseMarkdown("## コードブロック");
    TableOfContents toc;
    toc.BuildFromNodes(nodes);
    ASSERT_EQ(toc.GetEntries().size(), 1u);
    EXPECT_EQ(toc.GetEntries()[0].anchor_id, L"コードブロック");
}

TEST(Toc, RebuildClearsPrevious)
{
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

TEST(Toc, HitTestValidIndex)
{
    auto nodes = ParseMarkdown("# A\n\n## B\n\n### C");
    TableOfContents toc;
    toc.BuildFromNodes(nodes);

    EXPECT_EQ(toc.HitTest(0.0f, 28.0f), 0);
    EXPECT_EQ(toc.HitTest(28.0f, 28.0f), 1);
    EXPECT_EQ(toc.HitTest(56.0f, 28.0f), 2);
}

TEST(Toc, HitTestOutOfRange)
{
    auto nodes = ParseMarkdown("# A");
    TableOfContents toc;
    toc.BuildFromNodes(nodes);

    EXPECT_EQ(toc.HitTest(-1.0f, 28.0f), -1);
    EXPECT_EQ(toc.HitTest(100.0f, 28.0f), -1);
}

TEST(Toc, HitTestZeroItemHeight)
{
    auto nodes = ParseMarkdown("# A");
    TableOfContents toc;
    toc.BuildFromNodes(nodes);
    EXPECT_EQ(toc.HitTest(10.0f, 0.0f), -1);
}

TEST(Toc, HitTestEmpty)
{
    TableOfContents toc;
    EXPECT_EQ(toc.HitTest(0.0f, 28.0f), -1);
}

// ---- 追加エッジケース ----

TEST(Toc, DuplicateHeadingText)
{
    auto nodes = ParseMarkdown("# Title\n\nSome text\n\n# Title\n\nMore text\n\n## Title");
    TableOfContents toc;
    toc.BuildFromNodes(nodes);
    ASSERT_EQ(toc.GetEntries().size(), 3u);
    // すべて同じテキストを持つべき
    for (const auto& entry : toc.GetEntries()) {
        EXPECT_EQ(entry.text, L"Title");
    }
    // ただしanchor_idは一意であるべき（パーサーリファクタリング後）
    EXPECT_NE(toc.GetEntries()[0].anchor_id, toc.GetEntries()[1].anchor_id);
}

TEST(Toc, ManyHeadings)
{
    std::string md;
    for (int i = 0; i < 100; i++) {
        md += "## Heading " + std::to_string(i) + "\n\ntext\n\n";
    }
    auto nodes = ParseMarkdown(md);
    TableOfContents toc;
    toc.BuildFromNodes(nodes);
    EXPECT_EQ(toc.GetEntries().size(), 100u);
}

TEST(Toc, HeadingLevelsPreserved)
{
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

TEST(Toc, HitTestBoundary)
{
    auto nodes = ParseMarkdown("# A\n\n## B");
    TableOfContents toc;
    toc.BuildFromNodes(nodes);
    // アイテム間の境界上ちょうどの位置
    EXPECT_EQ(toc.HitTest(27.9f, 28.0f), 0);
    EXPECT_EQ(toc.HitTest(28.0f, 28.0f), 1);
}

TEST(Toc, HitTestNegativeItemHeight)
{
    auto nodes = ParseMarkdown("# A");
    TableOfContents toc;
    toc.BuildFromNodes(nodes);
    EXPECT_EQ(toc.HitTest(10.0f, -1.0f), -1);
}

// ---- node_index ----

TEST(Toc, NodeIndexRecorded)
{
    auto nodes = ParseMarkdown("Para\n\n# First\n\nMore text\n\n## Second");
    TableOfContents toc;
    toc.BuildFromNodes(nodes);
    ASSERT_EQ(toc.GetEntries().size(), 2u);
    // 最初のノード(Para)はインデックス0、# FirstはParagraphの次のノード
    EXPECT_GE(toc.GetEntries()[0].node_index, 0);
    EXPECT_LT(toc.GetEntries()[0].node_index, static_cast<int>(nodes.size()));
    EXPECT_GE(toc.GetEntries()[1].node_index, 0);
    EXPECT_GT(toc.GetEntries()[1].node_index, toc.GetEntries()[0].node_index);
}

TEST(Toc, NodeIndexOrderPreserved)
{
    auto nodes = ParseMarkdown("# A\n\n## B\n\n### C");
    TableOfContents toc;
    toc.BuildFromNodes(nodes);
    ASSERT_EQ(toc.GetEntries().size(), 3u);
    for (size_t i = 1; i < toc.GetEntries().size(); ++i) {
        EXPECT_GT(toc.GetEntries()[i].node_index, toc.GetEntries()[i - 1].node_index);
    }
}

// ---- FindActiveIndex ----

TEST(Toc, FindActiveIndexEmpty)
{
    TableOfContents toc;
    LayoutCache cache;
    EXPECT_EQ(toc.FindActiveIndex(cache, 0.0f), -1);
}

TEST(Toc, FindActiveIndexBeforeFirstHeading)
{
    auto nodes = ParseMarkdown("Para\n\n# Title");
    TableOfContents toc;
    toc.BuildFromNodes(nodes);

    LayoutCache cache;
    cache.Resize(nodes.size());
    // 見出しノードのy_positionを200に設定
    int heading_idx = toc.GetEntries()[0].node_index;
    cache[heading_idx].y_position = 200.0f;
    cache[heading_idx].height = 30.0f;

    // scroll_y=0 は見出しより前 → -1
    EXPECT_EQ(toc.FindActiveIndex(cache, 0.0f), -1);
}

TEST(Toc, FindActiveIndexAtHeading)
{
    auto nodes = ParseMarkdown("# First\n\nText\n\n## Second");
    TableOfContents toc;
    toc.BuildFromNodes(nodes);

    LayoutCache cache;
    cache.Resize(nodes.size());
    int first_idx = toc.GetEntries()[0].node_index;
    int second_idx = toc.GetEntries()[1].node_index;
    cache[first_idx].y_position = 10.0f;
    cache[first_idx].height = 30.0f;
    cache[second_idx].y_position = 200.0f;
    cache[second_idx].height = 25.0f;

    // scroll_y=10 は最初の見出しのy_position丁度 → 0
    EXPECT_EQ(toc.FindActiveIndex(cache, 10.0f), 0);
    // scroll_y=100 は最初の見出しの後、2番目の前 → 0
    EXPECT_EQ(toc.FindActiveIndex(cache, 100.0f), 0);
    // scroll_y=200 は2番目の見出しのy_position丁度 → 1
    EXPECT_EQ(toc.FindActiveIndex(cache, 200.0f), 1);
    // scroll_y=300 は2番目の見出しの後 → 1
    EXPECT_EQ(toc.FindActiveIndex(cache, 300.0f), 1);
}

TEST(Toc, FindActiveIndexManyHeadings)
{
    std::string md;
    for (int i = 0; i < 10; ++i) {
        md += "## H" + std::to_string(i) + "\n\nText\n\n";
    }
    auto nodes = ParseMarkdown(md);
    TableOfContents toc;
    toc.BuildFromNodes(nodes);
    ASSERT_EQ(toc.GetEntries().size(), 10u);

    LayoutCache cache;
    cache.Resize(nodes.size());
    // 各見出しノードのy_positionを 100*i に設定
    for (int i = 0; i < 10; ++i) {
        int ni = toc.GetEntries()[i].node_index;
        cache[ni].y_position = static_cast<float>(i * 100);
        cache[ni].height = 30.0f;
    }

    EXPECT_EQ(toc.FindActiveIndex(cache, 0.0f), 0);
    EXPECT_EQ(toc.FindActiveIndex(cache, 450.0f), 4);
    EXPECT_EQ(toc.FindActiveIndex(cache, 999.0f), 9);
}

// margin付きのFindActiveIndex: TOCリンククリック時に正しい見出しがアクティブになることを確認
TEST(Toc, FindActiveIndexWithMargin)
{
    auto nodes = ParseMarkdown("# First\n\nText\n\n## Second");
    TableOfContents toc;
    toc.BuildFromNodes(nodes);

    LayoutCache cache;
    cache.Resize(nodes.size());
    int first_idx = toc.GetEntries()[0].node_index;
    int second_idx = toc.GetEntries()[1].node_index;
    cache[first_idx].y_position = 100.0f;
    cache[first_idx].height = 30.0f;
    cache[second_idx].y_position = 300.0f;
    cache[second_idx].height = 25.0f;

    // NavigateToAnchorが target_y = y_position - margin でスクロールする想定
    // margin=50 のとき、scroll_y=250 (= 300 - 50) で2番目の見出しがアクティブになるべき
    float margin = 50.0f;
    float scroll_after_click = 300.0f - margin;  // = 250
    // margin無しでは scroll_y=250 < y_position=300 なので1番目が選ばれてしまう
    EXPECT_EQ(toc.FindActiveIndex(cache, scroll_after_click), 0);
    // margin有りでは threshold=300 >= y_position=300 なので2番目が正しく選ばれる
    EXPECT_EQ(toc.FindActiveIndex(cache, scroll_after_click, margin), 1);
}
