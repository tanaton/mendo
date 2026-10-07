// 実セルレイアウト (DWriteTestBase) を使う検索マッチ Y のテスト。
// 表の複数行セルでは (row, col, start) 順に並ぶマッチの Y が単調にならないため、
// モックの固定行高では再現できない。
#include <gtest/gtest.h>
#include "command_generator.h"
#include "draw_command.h"
#include "dwrite_test_base.h"
#include "search_state.h"
#include <algorithm>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace {

// 1 列目の長文が折り返して "hit" が 3 行目以降に来る一方、同じ行の 2 列目の "hit" は 1 行目にある。
constexpr std::string_view TABLE_MD =
    "| first column | second |\n"
    "|---|---|\n"
    "| this cell has enough words to wrap over several lines before the final hit | hit |\n"
    "| hit | plain |\n"
    "| more words here so the cell wraps again and again before hit | another hit |\n\n";
constexpr float VIEWPORT_W = 420.0f;

// 表の前後のマッチ数を変え、二分探索の中央が表内の逆転箇所に当たる配置を網羅する。
std::string MakeDoc(int leading, int trailing)
{
    std::string md;
    for (int i = 0; i < leading; i++) {
        md += "leading hit paragraph\n\n";
    }
    md += TABLE_MD;
    for (int i = 0; i < trailing; i++) {
        md += "trailing hit paragraph\n\n";
    }
    return md;
}

class SearchMatchYTest : public DWriteTestBase {
protected:
    struct Doc {
        ParsedLayout pl;
        SearchState search;
        std::vector<float> match_ys;
    };

    void Load(Doc& d, const std::string& md)
    {
        d.pl = ParseAndLayout(md, VIEWPORT_W);
        d.search.SetQuery("hit");
        d.search.ExecuteSearch(d.pl.nodes);
        for (const auto& m : d.search.GetMatches()) {
            const auto node = static_cast<size_t>(m.node_index);
            d.match_ys.push_back(d.pl.cache[node].GetMatchYRange(m.table_row, m.table_col, m.start_w, d.pl.cache.Top(node)).first);
        }
    }
};

// 仕様: マッチ順で最初に match_y >= scroll_y となるマッチ、無ければ先頭。
int LinearNearest(const std::vector<float>& ys, float scroll_y)
{
    const auto it = std::ranges::find_if(ys, [scroll_y](float y) { return y >= scroll_y; });
    return (it != ys.end()) ? static_cast<int>(it - ys.begin()) : 0;
}

} // namespace

TEST_F(SearchMatchYTest, SetCurrentMatchNearAgreesWithLinearScan)
{
    for (int leading = 0; leading <= 6; leading++) {
        for (int trailing = 0; trailing <= 2; trailing++) {
            SCOPED_TRACE("leading=" + std::to_string(leading) + " trailing=" + std::to_string(trailing));
            Doc d;
            Load(d, MakeDoc(leading, trailing));
            // 前提: 表の同一行内でマッチ順と Y 順が逆転している (二分探索の単調性が崩れる状況)。
            ASSERT_FALSE(std::ranges::is_sorted(d.match_ys));

            std::vector<float> probes{ -10.0f, d.match_ys.back() + 100.0f };
            for (const float y : d.match_ys) {
                for (const float delta : { -2.0f, -0.5f, 0.0f, 0.5f, 2.0f }) {
                    probes.push_back(y + delta);
                }
            }
            for (const float y : probes) {
                d.search.SetCurrentMatchNear(y, d.pl.cache);
                ASSERT_EQ(d.search.GetCurrentMatchIndex(), LinearNearest(d.match_ys, y)) << "scroll_y=" << y;
            }
        }
    }
}

// スクロール位置合わせに使う GetMatchYRange の Y は、実際に描かれるハイライト矩形の上端と一致する。
TEST_F(SearchMatchYTest, MatchYRangeAgreesWithDrawnHighlight)
{
    Doc d;
    Load(d, MakeDoc(1, 1));
    CommandGenerator gen;
    std::pmr::vector<DWRITE_HIT_TEST_METRICS> hit_buf;
    gen.SetTheme(&theme_);
    gen.SetFormats({});
    gen.SetHitTestBuffer(&hit_buf);

    const auto& matches = d.search.GetMatches();
    for (int i = 0; i < static_cast<int>(matches.size()); i++) {
        SCOPED_TRACE(i);
        gen.SetSearchMatches(&matches, i, d.search.GetGeneration());
        const auto& cmds = gen.GenerateMdPane(d.pl.nodes, d.pl.cache, PaneRect{ 0.0f, 0.0f, 4000.0f, 4000.0f }, 0.0f, TextSelection{});
        std::optional<float> drawn_top;
        for (const auto& c : cmds) {
            if (const auto* r = std::get_if<FillRectCmd>(&c); r && r->brush_id == BrushId::SearchHighlightCurrent) {
                drawn_top = drawn_top ? std::min(*drawn_top, r->rect.top) : r->rect.top;
            }
        }
        ASSERT_TRUE(drawn_top.has_value());
        EXPECT_NEAR(d.match_ys[static_cast<size_t>(i)], *drawn_top, 0.01f);
    }
}
