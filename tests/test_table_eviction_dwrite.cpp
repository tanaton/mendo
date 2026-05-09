// issue#210: スクロールでテーブルセル描画が欠落する不具合の再発防止テスト。
// EvictInvisibleTableRows で行単位 evict された entry が dirty 化され、
// 再可視化時の MeasureTable で null セルが復元される一連のフローを検証する。
#include <gtest/gtest.h>
#include "dwrite_test_base.h"
#include <string>

namespace {

class TableEvictionDWriteTest : public DWriteTestBase {
protected:
    static std::string MakeBigTableMd(size_t row_count)
    {
        std::string md = "| A | B | C |\n|---|---|---|\n";
        for (size_t i = 0; i < row_count; ++i) {
            md += "| r" + std::to_string(i) + "c0 ";
            md += "| r" + std::to_string(i) + "c1 ";
            md += "| r" + std::to_string(i) + "c2 |\n";
        }
        return md;
    }

    static int FindTableNode(const std::pmr::vector<Node>& nodes)
    {
        for (size_t i = 0; i < nodes.size(); ++i) {
            if (nodes[i].type == NodeType::Table) {
                return static_cast<int>(i);
            }
        }
        return -1;
    }

    static size_t CountNonNullCells(const TableLayoutData& tl)
    {
        size_t n = 0;
        for (const auto& cl : tl.cell_layouts) {
            if (cl) {
                ++n;
            }
        }
        return n;
    }
};

} // namespace

// 範囲外行を evict すると entry が dirty 化されフラグが立ち、null セルが発生する。
TEST_F(TableEvictionDWriteTest, EvictInvisibleRowsMarksDirtyAndCreatesNullCells)
{
    auto pl = ParseAndLayout(MakeBigTableMd(40));
    const int idx = FindTableNode(pl.nodes);
    ASSERT_GE(idx, 0);
    auto& entry = pl.cache[idx];
    ASSERT_TRUE(entry.has_table_layout());
    auto& tl = *entry.table_layout;

    const size_t total_cells = tl.cell_layouts.size();
    ASSERT_GT(total_cells, 0u);
    ASSERT_EQ(CountNonNullCells(tl), total_cells);
    entry.layout_dirty = false;
    tl.cells_partially_evicted = false;

    // entry は text_top=margin_top あたりから始まるので、
    // [text_top, text_top + viewport_height] を可視範囲としても
    // テーブル後半は範囲外になる。buffer は 0 にして外行を確実に evict する。
    const float vp_top = entry.text_top;
    const float vp_bottom = entry.text_top + 80.0f;
    pl.cache.EvictInvisibleTableRows(vp_top, vp_bottom, 0.0f);

    EXPECT_TRUE(tl.cells_partially_evicted);
    EXPECT_TRUE(entry.layout_dirty);
    EXPECT_LT(CountNonNullCells(tl), total_cells)
        << "範囲外行のセルが Reset されているはず";
}

// 全行が viewport 内なら evict は発生せず dirty 化もしない。
TEST_F(TableEvictionDWriteTest, EvictInvisibleRowsNoOpWhenAllVisible)
{
    auto pl = ParseAndLayout(MakeBigTableMd(5));
    const int idx = FindTableNode(pl.nodes);
    ASSERT_GE(idx, 0);
    auto& entry = pl.cache[idx];
    ASSERT_TRUE(entry.has_table_layout());
    auto& tl = *entry.table_layout;

    const size_t total_cells = tl.cell_layouts.size();
    entry.layout_dirty = false;
    tl.cells_partially_evicted = false;

    pl.cache.EvictInvisibleTableRows(entry.text_top - 100.0f, entry.text_top + entry.height + 100.0f, 50.0f);

    EXPECT_FALSE(tl.cells_partially_evicted);
    EXPECT_FALSE(entry.layout_dirty);
    EXPECT_EQ(CountNonNullCells(tl), total_cells);
}

// evict 後の再 measure で null セルが復元されフラグがクリアされる。
TEST_F(TableEvictionDWriteTest, MeasureAfterEvictionRestoresNullCells)
{
    auto pl = ParseAndLayout(MakeBigTableMd(40));
    const int idx = FindTableNode(pl.nodes);
    ASSERT_GE(idx, 0);
    auto& entry = pl.cache[idx];
    ASSERT_TRUE(entry.has_table_layout());
    auto& tl = *entry.table_layout;

    const size_t total_cells = tl.cell_layouts.size();
    ASSERT_GT(total_cells, 0u);

    pl.cache.EvictInvisibleTableRows(entry.text_top, entry.text_top + 80.0f, 0.0f);
    ASSERT_TRUE(tl.cells_partially_evicted);
    ASSERT_LT(CountNonNullCells(tl), total_cells);

    // EnsureVisibleLayout に相当する経路: dirty な entry に対して再 measure する。
    const float content_width = theme_.ContentWidth(800.0f);
    measurer_.MeasureNode(pl.nodes[idx], entry, content_width);

    EXPECT_FALSE(tl.cells_partially_evicted);
    EXPECT_FALSE(entry.layout_dirty);
    EXPECT_EQ(CountNonNullCells(tl), total_cells)
        << "RestoreNullCellLayouts で evict されたセルが復元されているはず";
}
