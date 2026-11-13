#include <gtest/gtest.h>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
#include "layout_cache.h"
#include "layout_computer.h"
#include "parser.h"
#include "test_nodes.h"

using layout::FindActiveTocIndex;

namespace {

struct ParsedToc {
    std::vector<Node> nodes;
    std::vector<TocEntry> toc;
};

// パーサが見出しの終わりで積んだ目次をそのまま使う。
ParsedToc ParseToc(std::string_view markdown)
{
    auto storage = ParseMarkdown(markdown);
    std::vector<Node> nodes(storage.nodes.begin(), storage.nodes.end());
    auto toc = std::move(storage.indices.toc_entries);
    test_nodes_detail::KeepAlive(std::move(storage));
    return { std::move(nodes), std::move(toc) };
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
        items.push_back({ parsed.nodes[entry.node_index].heading_level, std::string{ parsed.nodes[entry.node_index].GetText() } });
    }
    return items;
}

// 目次の i 番目の見出しノードを tops[i] に置き、高さを height にした LayoutCache。
LayoutCache MakeHeadingCache(const ParsedToc& parsed, const std::vector<float>& tops, float height = 30.0f)
{
    LayoutCache cache(parsed.nodes.size());
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

// ---- FindActiveIndex ----

// scroll_y + margin 以前に始まる最後の見出しがアクティブ。最初の見出しより前や目次が空なら -1。
// margin は目次のクリックで見出しの margin 上へスクロールした位置でも、その見出しを選ぶため。
TEST(Toc, FindActiveIndex)
{
    std::string many;
    std::vector<float> many_tops;
    for (int i = 0; i < 10; ++i) {
        many += "## H" + std::to_string(i) + "\n\nText\n\n";
        many_tops.push_back(static_cast<float>(i) * 100.0f);
    }
    struct Case {
        std::string md;
        std::vector<float> tops;
        float scroll_y;
        float margin;
        int expected;
    };
    const Case kCases[] = {
        { "Para", {}, 0.0f, 0.0f, -1 },
        { "Para\n\n# Title", { 200.0f }, 0.0f, 0.0f, -1 },
        { "# First\n\nText\n\n## Second", { 10.0f, 200.0f }, 10.0f, 0.0f, 0 },
        { "# First\n\nText\n\n## Second", { 10.0f, 200.0f }, 100.0f, 0.0f, 0 },
        { "# First\n\nText\n\n## Second", { 10.0f, 200.0f }, 200.0f, 0.0f, 1 },
        { "# First\n\nText\n\n## Second", { 10.0f, 200.0f }, 300.0f, 0.0f, 1 },
        { many, many_tops, 0.0f, 0.0f, 0 },
        { many, many_tops, 450.0f, 0.0f, 4 },
        { many, many_tops, 999.0f, 0.0f, 9 },
        { "# First\n\nText\n\n## Second", { 100.0f, 300.0f }, 250.0f, 0.0f, 0 },
        { "# First\n\nText\n\n## Second", { 100.0f, 300.0f }, 250.0f, 50.0f, 1 },
    };
    for (const auto& c : kCases) {
        SCOPED_TRACE(::testing::Message() << c.md.substr(0, 20) << " scroll=" << c.scroll_y << " margin=" << c.margin);
        const auto parsed = ParseToc(c.md);
        ASSERT_EQ(parsed.toc.size(), c.tops.size());
        const auto cache = MakeHeadingCache(parsed, c.tops);
        EXPECT_EQ(FindActiveTocIndex(parsed.toc, cache, c.scroll_y, c.margin), c.expected);
    }
}
