#include <gtest/gtest.h>
#include <initializer_list>
#include <memory_resource>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
#include "layout_cache.h"
#include "layout_queries.h"
#include "parser.h"
#include "test_nodes.h"

namespace {

struct ParsedToc {
    std::pmr::vector<Node> nodes;
    std::pmr::vector<TocEntry> toc;
};

// パーサが見出しの終わりで積んだ目次をそのまま使う。
ParsedToc ParseToc(std::string_view markdown)
{
    auto result = ParseMarkdown(markdown);
    auto storage = result.TakeNodes();
    auto nodes = std::move(storage.nodes);
    test_nodes_detail::KeepAlive(std::move(storage));
    return { std::move(nodes), std::move(result.toc_entries) };
}

struct TocItem {
    int level;
    std::string text;
    bool operator==(const TocItem&) const = default;
    friend void PrintTo(const TocItem& item, std::ostream* os)
    {
        *os << "{" << item.level << ", " << item.text << "}";
    }
};

std::vector<TocItem> TocItems(const ParsedToc& parsed)
{
    std::vector<TocItem> items;
    for (const auto& entry : parsed.toc) {
        items.push_back({ entry.heading_level, std::string{ parsed.nodes[entry.node_index].GetText() } });
    }
    return items;
}

// 目次の i 番目の見出しノードを tops[i] に置き、高さを height にした LayoutCache。
LayoutCache MakeHeadingCache(const ParsedToc& parsed, std::initializer_list<float> tops, float height = 30.0f)
{
    LayoutCache cache;
    cache.Resize(parsed.nodes.size());
    size_t i = 0;
    for (const float top : tops) {
        const int node_index = parsed.toc[i++].node_index;
        cache.SetTop(node_index, top);
        cache[node_index].height = height;
    }
    return cache;
}

} // namespace

TEST(Toc, NoEntriesWithoutHeadings)
{
    for (const std::string_view md : { std::string_view{ "" }, std::string_view{ "Just a paragraph.\n\n- List item\n\n> Quote" } }) {
        SCOPED_TRACE(md);
        EXPECT_TRUE(ParseToc(md).toc.empty());
    }
}

// 見出しだけを文書順に、レベルとテキストを保って並べる。同じテキストの見出しもそれぞれ載せる。
TEST(Toc, EntriesFollowHeadingsInOrder)
{
    struct Case {
        std::string_view md;
        std::vector<TocItem> expected;
    };
    const Case kCases[] = {
        { "# Title", { { 1, "Title" } } },
        { "## Hello World", { { 2, "Hello World" } } },
        { "# First\n\n## Second\n\n### Third\n\nParagraph\n\n## Another",
          { { 1, "First" }, { 2, "Second" }, { 3, "Third" }, { 2, "Another" } } },
        { "# L1\n\n## L2\n\n### L3\n\n#### L4\n\n##### L5\n\n###### L6",
          { { 1, "L1" }, { 2, "L2" }, { 3, "L3" }, { 4, "L4" }, { 5, "L5" }, { 6, "L6" } } },
        { "# Title\n\nSome text\n\n# Title\n\nMore text\n\n## Title", { { 1, "Title" }, { 1, "Title" }, { 2, "Title" } } },
        { "Para\n\n# First\n\nMore text\n\n## Second", { { 1, "First" }, { 2, "Second" } } },
    };
    for (const auto& c : kCases) {
        SCOPED_TRACE(c.md);
        const auto parsed = ParseToc(c.md);
        EXPECT_EQ(TocItems(parsed), c.expected);
        int prev = -1;
        for (const auto& entry : parsed.toc) {
            ASSERT_LT(entry.node_index, static_cast<int>(parsed.nodes.size()));
            EXPECT_EQ(parsed.nodes[entry.node_index].type, NodeType::Heading);
            EXPECT_GT(entry.node_index, prev);
            prev = entry.node_index;
        }
    }
}

TEST(Toc, ManyHeadings)
{
    std::string md;
    for (int i = 0; i < 100; i++) {
        md += "## Heading " + std::to_string(i) + "\n\ntext\n\n";
    }
    EXPECT_EQ(ParseToc(md).toc.size(), 100u);
}

// ---- FindActiveIndex ----

TEST(Toc, FindActiveIndexEmpty)
{
    std::pmr::vector<TocEntry> toc;
    LayoutCache cache;
    EXPECT_EQ(FindActiveTocIndex(toc, cache, 0.0f), -1);
}

TEST(Toc, FindActiveIndexBeforeFirstHeading)
{
    const auto parsed = ParseToc("Para\n\n# Title");
    const auto cache = MakeHeadingCache(parsed, { 200.0f });
    EXPECT_EQ(FindActiveTocIndex(parsed.toc, cache, 0.0f), -1);
}

// scroll_y 以前に始まる最後の見出しがアクティブ。
TEST(Toc, FindActiveIndexAtHeading)
{
    const auto parsed = ParseToc("# First\n\nText\n\n## Second");
    const auto cache = MakeHeadingCache(parsed, { 10.0f, 200.0f });
    EXPECT_EQ(FindActiveTocIndex(parsed.toc, cache, 10.0f), 0);
    EXPECT_EQ(FindActiveTocIndex(parsed.toc, cache, 100.0f), 0);
    EXPECT_EQ(FindActiveTocIndex(parsed.toc, cache, 200.0f), 1);
    EXPECT_EQ(FindActiveTocIndex(parsed.toc, cache, 300.0f), 1);
}

TEST(Toc, FindActiveIndexManyHeadings)
{
    std::string md;
    for (int i = 0; i < 10; ++i) {
        md += "## H" + std::to_string(i) + "\n\nText\n\n";
    }
    const auto parsed = ParseToc(md);
    ASSERT_EQ(parsed.toc.size(), 10u);
    const auto cache = MakeHeadingCache(parsed, { 0.0f, 100.0f, 200.0f, 300.0f, 400.0f, 500.0f, 600.0f, 700.0f, 800.0f, 900.0f });

    EXPECT_EQ(FindActiveTocIndex(parsed.toc, cache, 0.0f), 0);
    EXPECT_EQ(FindActiveTocIndex(parsed.toc, cache, 450.0f), 4);
    EXPECT_EQ(FindActiveTocIndex(parsed.toc, cache, 999.0f), 9);
}

// margin付きのFindActiveIndex: TOCリンククリック時に正しい見出しがアクティブになることを確認
TEST(Toc, FindActiveIndexWithMargin)
{
    const auto parsed = ParseToc("# First\n\nText\n\n## Second");
    const auto cache = MakeHeadingCache(parsed, { 100.0f, 300.0f });

    // NavigateToAnchorが target_y = y_position - margin でスクロールする想定
    // margin=50 のとき、scroll_y=250 (= 300 - 50) で2番目の見出しがアクティブになるべき
    constexpr float margin = 50.0f;
    constexpr float scroll_after_click = 300.0f - margin;
    // margin無しでは scroll_y=250 < y_position=300 なので1番目が選ばれてしまう
    EXPECT_EQ(FindActiveTocIndex(parsed.toc, cache, scroll_after_click), 0);
    // margin有りでは threshold=300 >= y_position=300 なので2番目が正しく選ばれる
    EXPECT_EQ(FindActiveTocIndex(parsed.toc, cache, scroll_after_click, margin), 1);
}
