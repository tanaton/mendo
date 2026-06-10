// issue#210: スクロールでテーブルセル描画が欠落する不具合の再発防止テスト。
// EvictInvisibleTableRows で行単位 evict された entry が dirty 化され、
// 再可視化時の MeasureTable で null セルが復元される一連のフローを検証する。
#include <gtest/gtest.h>
#include "dwrite_test_base.h"
#include <array>
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
    pl.cache.EvictInvisibleTableRows(std::array{ static_cast<size_t>(idx) }, vp_top, vp_bottom, 0.0f);

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

    pl.cache.EvictInvisibleTableRows(std::array{ static_cast<size_t>(idx) }, entry.text_top - 100.0f, entry.text_top + entry.height + 100.0f, 50.0f);

    EXPECT_FALSE(tl.cells_partially_evicted);
    EXPECT_FALSE(entry.layout_dirty);
    EXPECT_EQ(CountNonNullCells(tl), total_cells);
}

// evict 後にデフォルト引数 (viewport 全範囲) で再 measure すると、null セル全てが
// 復元されフラグがクリアされる (リサイズ等の全範囲計測ケース)。
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

    pl.cache.EvictInvisibleTableRows(std::array{ static_cast<size_t>(idx) }, entry.text_top, entry.text_top + 80.0f, 0.0f);
    ASSERT_TRUE(tl.cells_partially_evicted);
    ASSERT_LT(CountNonNullCells(tl), total_cells);

    const float content_width = theme_.ContentWidth(800.0f);
    measurer_.MeasureNode(pl.nodes[idx], entry, content_width);

    EXPECT_FALSE(tl.cells_partially_evicted);
    EXPECT_FALSE(entry.layout_dirty);
    EXPECT_EQ(CountNonNullCells(tl), total_cells)
        << "RestoreNullCellLayouts で evict されたセルが復元されているはず";
}

// viewport 範囲を指定した MeasureNode は範囲内の行だけ復元し、範囲外は null のまま残す。
// dirty / cells_partially_evicted は維持され、次回スクロール時の追加復元を促す。
TEST_F(TableEvictionDWriteTest, MeasureWithViewportRestoresOnlyVisibleRows)
{
    auto pl = ParseAndLayout(MakeBigTableMd(60));
    const int idx = FindTableNode(pl.nodes);
    ASSERT_GE(idx, 0);
    auto& entry = pl.cache[idx];
    ASSERT_TRUE(entry.has_table_layout());
    auto& tl = *entry.table_layout;

    const size_t total_cells = tl.cell_layouts.size();
    ASSERT_GT(total_cells, 0u);

    // テーブル全体を一旦 evict (viewport より遠い範囲) → ほぼ全セルが null になる。
    pl.cache.EvictInvisibleTableRows(std::array{ static_cast<size_t>(idx) }, entry.text_top - 10000.0f, entry.text_top - 9000.0f, 0.0f);
    const size_t after_evict = CountNonNullCells(tl);
    ASSERT_LT(after_evict, total_cells);
    ASSERT_TRUE(tl.cells_partially_evicted);

    // テーブル先頭 80px だけを viewport として measure。範囲内行のみ復元される。
    const float content_width = theme_.ContentWidth(800.0f);
    const MeasureViewportRange vp{ entry.text_top, entry.text_top + 80.0f };
    measurer_.MeasureNode(pl.nodes[idx], entry, content_width, nullptr, vp);

    const size_t after_measure = CountNonNullCells(tl);
    EXPECT_GT(after_measure, after_evict)
        << "viewport 範囲内のセルは復元されているはず";
    EXPECT_LT(after_measure, total_cells)
        << "viewport 範囲外のセルは null のまま残っているはず";
    EXPECT_TRUE(tl.cells_partially_evicted)
        << "復元未完了なのでフラグは維持される";
    EXPECT_TRUE(entry.layout_dirty)
        << "次回スクロール時に再 measure させるため dirty を維持";
}

// 部分復元 → 別 viewport で追加復元 → 最終的に全セルが揃いフラグがクリアされる。
TEST_F(TableEvictionDWriteTest, ProgressivelyRestoresAcrossScrolls)
{
    auto pl = ParseAndLayout(MakeBigTableMd(60));
    const int idx = FindTableNode(pl.nodes);
    ASSERT_GE(idx, 0);
    auto& entry = pl.cache[idx];
    ASSERT_TRUE(entry.has_table_layout());
    auto& tl = *entry.table_layout;

    const size_t total_cells = tl.cell_layouts.size();
    pl.cache.EvictInvisibleTableRows(std::array{ static_cast<size_t>(idx) }, entry.text_top - 10000.0f, entry.text_top - 9000.0f, 0.0f);
    ASSERT_LT(CountNonNullCells(tl), total_cells);

    const float content_width = theme_.ContentWidth(800.0f);
    // 1 回目: テーブル上半分を viewport にして measure
    const MeasureViewportRange vp1{ entry.text_top, entry.text_top + entry.height * 0.5f };
    measurer_.MeasureNode(pl.nodes[idx], entry, content_width, nullptr, vp1);
    EXPECT_TRUE(tl.cells_partially_evicted);

    // 2 回目: テーブル全体を覆う viewport で再 measure
    const MeasureViewportRange vp2{ entry.text_top, entry.text_top + entry.height };
    measurer_.MeasureNode(pl.nodes[idx], entry, content_width, nullptr, vp2);

    EXPECT_EQ(CountNonNullCells(tl), total_cells)
        << "全範囲を覆う viewport で全セル復元されるはず";
    EXPECT_FALSE(tl.cells_partially_evicted);
    EXPECT_FALSE(entry.layout_dirty);
}
