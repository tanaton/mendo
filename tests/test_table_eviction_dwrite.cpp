// issue#210: スクロールでテーブルセル描画が欠落する不具合の再発防止テスト。
// EvictInvisibleTableRows で行単位 evict された行が、dirty を経由せず
// RestoreEvictedTableRows / EnsureVisibleLayout で可視行だけ復元される一連のフローを検証する。
#include <gtest/gtest.h>
#include "dwrite_test_base.h"
#include "task_scheduler.h"
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

    // テーブル全体を一旦 evict (viewport より遠い範囲) → 全セルが null になる。
    static void EvictWholeTable(LayoutCache& cache, int idx)
    {
        const float far_top = cache.Top(idx) - 10000.0f;
        cache.EvictInvisibleTableRows(std::array{ static_cast<size_t>(idx) }, far_top, far_top + 1000.0f, 0.0f);
    }

    float ContentWidth() const
    {
        return theme_.ContentWidth(800.0f);
    }
};

} // namespace

// 範囲外行を evict すると null セルが発生するが、dirty にはしない
// (dirty のままだと可視中に毎フレーム全行の再計測と effects 破棄が走る)。
TEST_F(TableEvictionDWriteTest, EvictInvisibleRowsCreatesNullCellsWithoutDirty)
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
    ASSERT_FALSE(entry.layout_dirty);

    pl.cache.EvictInvisibleTableRows(std::array{ static_cast<size_t>(idx) }, pl.cache.Top(idx), pl.cache.Top(idx) + 80.0f, 0.0f);

    EXPECT_TRUE(tl.HasEvictedRows());
    EXPECT_FALSE(entry.layout_dirty);
    EXPECT_LT(CountNonNullCells(tl), total_cells)
        << "範囲外行のセルが Reset されているはず";
}

// 全行が viewport 内なら evict は発生しない。
TEST_F(TableEvictionDWriteTest, EvictInvisibleRowsNoOpWhenAllVisible)
{
    auto pl = ParseAndLayout(MakeBigTableMd(5));
    const int idx = FindTableNode(pl.nodes);
    ASSERT_GE(idx, 0);
    auto& entry = pl.cache[idx];
    ASSERT_TRUE(entry.has_table_layout());
    auto& tl = *entry.table_layout;

    const size_t total_cells = tl.cell_layouts.size();
    pl.cache.EvictInvisibleTableRows(std::array{ static_cast<size_t>(idx) }, pl.cache.Top(idx) - 100.0f, pl.cache.Bottom(idx) + 100.0f, 50.0f);

    EXPECT_FALSE(tl.HasEvictedRows());
    EXPECT_FALSE(entry.layout_dirty);
    EXPECT_EQ(CountNonNullCells(tl), total_cells);
}

// 同じ幅での MeasureNode は幾何が不変なので超高速パスで抜け、全行を再計測しない。
TEST_F(TableEvictionDWriteTest, SameWidthMeasureTakesFastPathAfterEviction)
{
    auto pl = ParseAndLayout(MakeBigTableMd(40));
    const int idx = FindTableNode(pl.nodes);
    ASSERT_GE(idx, 0);
    auto& entry = pl.cache[idx];
    auto& tl = *entry.table_layout;
    const float height_before = entry.height;

    EvictWholeTable(pl.cache, idx);
    ASSERT_EQ(CountNonNullCells(tl), 0u);

    entry.layout_dirty = true;
    measurer_.MeasureNode(pl.nodes[idx], entry, ContentWidth());

    EXPECT_FALSE(entry.layout_dirty);
    EXPECT_EQ(CountNonNullCells(tl), 0u) << "幅不変なら MeasureNode はセルを作り直さない";
    EXPECT_TRUE(tl.HasEvictedRows());
    EXPECT_FLOAT_EQ(entry.height, height_before);
}

// RestoreEvictedTableRows は viewport 内の evict 行だけ復元し、範囲外は null のまま残す。
TEST_F(TableEvictionDWriteTest, RestoreRestoresOnlyVisibleRows)
{
    auto pl = ParseAndLayout(MakeBigTableMd(60));
    const int idx = FindTableNode(pl.nodes);
    ASSERT_GE(idx, 0);
    auto& entry = pl.cache[idx];
    auto& tl = *entry.table_layout;
    const size_t total_cells = tl.cell_layouts.size();
    const float height_before = entry.height;

    EvictWholeTable(pl.cache, idx);
    const size_t after_evict = CountNonNullCells(tl);
    ASSERT_LT(after_evict, total_cells);

    const MeasureViewportRange vp{ 0.0f, 80.0f };
    const auto result = measurer_.RestoreEvictedTableRows(pl.nodes[idx], entry, ContentWidth(), vp);

    EXPECT_TRUE(result.restored);
    EXPECT_FALSE(result.height_changed);
    const size_t after_restore = CountNonNullCells(tl);
    EXPECT_GT(after_restore, after_evict);
    EXPECT_LT(after_restore, total_cells);
    EXPECT_TRUE(tl.HasEvictedRows());
    EXPECT_FALSE(entry.layout_dirty);
    EXPECT_FLOAT_EQ(entry.height, height_before);
}

// 部分復元 → 別 viewport で追加復元 → 最終的に全セルが揃い evict 行がなくなる。
TEST_F(TableEvictionDWriteTest, ProgressivelyRestoresAcrossScrolls)
{
    auto pl = ParseAndLayout(MakeBigTableMd(60));
    const int idx = FindTableNode(pl.nodes);
    ASSERT_GE(idx, 0);
    auto& entry = pl.cache[idx];
    auto& tl = *entry.table_layout;
    const size_t total_cells = tl.cell_layouts.size();

    EvictWholeTable(pl.cache, idx);
    ASSERT_LT(CountNonNullCells(tl), total_cells);

    const MeasureViewportRange vp1{ 0.0f, entry.height * 0.5f };
    measurer_.RestoreEvictedTableRows(pl.nodes[idx], entry, ContentWidth(), vp1);
    EXPECT_TRUE(tl.HasEvictedRows());

    const MeasureViewportRange vp2{ 0.0f, entry.height };
    measurer_.RestoreEvictedTableRows(pl.nodes[idx], entry, ContentWidth(), vp2);

    EXPECT_EQ(CountNonNullCells(tl), total_cells);
    EXPECT_FALSE(tl.HasEvictedRows());
}

// 2 回目の evict は前回の生存範囲との差分だけを対象にし、復元済み行は再び evict できる。
TEST_F(TableEvictionDWriteTest, EvictIsDifferentialAgainstLiveRange)
{
    auto pl = ParseAndLayout(MakeBigTableMd(60));
    const int idx = FindTableNode(pl.nodes);
    ASSERT_GE(idx, 0);
    auto& entry = pl.cache[idx];
    auto& tl = *entry.table_layout;

    EvictWholeTable(pl.cache, idx);
    EXPECT_EQ(tl.live_row_begin, tl.live_row_end) << "全行 evict 後は生存範囲が空";

    const MeasureViewportRange vp{ 0.0f, 80.0f };
    measurer_.RestoreEvictedTableRows(pl.nodes[idx], entry, ContentWidth(), vp);
    const size_t restored_cells = CountNonNullCells(tl);
    ASSERT_GT(restored_cells, 0u);
    EXPECT_LT(tl.live_row_begin, tl.live_row_end);

    EvictWholeTable(pl.cache, idx);
    EXPECT_EQ(CountNonNullCells(tl), 0u) << "復元済み行は生存範囲に入り、再 evict される";
}

// ノード単位 evict でもテーブルは列幅・行高さを残し、同じ幅なら再計測不要で戻れる。
TEST_F(TableEvictionDWriteTest, NodeEvictionKeepsTableGeometry)
{
    auto pl = ParseAndLayout("para\n\n" + MakeBigTableMd(40));
    const int idx = FindTableNode(pl.nodes);
    ASSERT_GE(idx, 0);
    auto& entry = pl.cache[idx];
    ASSERT_TRUE(entry.has_table_layout());
    const float height_before = entry.height;
    const auto col_widths_before = entry.table_layout->col_widths;

    // keep 範囲を先頭ノードだけにしてテーブルを evict 対象にする。
    pl.cache.EvictTextLayouts(0, 1);

    ASSERT_TRUE(entry.has_table_layout()) << "幾何ごと破棄すると再表示時に全セル再構築になる";
    auto& tl = *entry.table_layout;
    EXPECT_EQ(CountNonNullCells(tl), 0u);
    EXPECT_TRUE(tl.HasEvictedRows());
    EXPECT_EQ(tl.col_widths, col_widths_before);
    EXPECT_FALSE(entry.layout_dirty);

    const MeasureViewportRange vp{ 0.0f, entry.height };
    const auto result = measurer_.RestoreEvictedTableRows(pl.nodes[idx], entry, ContentWidth(), vp);
    EXPECT_TRUE(result.restored);
    EXPECT_EQ(CountNonNullCells(tl), tl.cell_layouts.size());
    EXPECT_FLOAT_EQ(entry.height, height_before);
}

// 幅変更で evict 中だった行は旧幅の行高さのまま残り、復元時に実測へ補正される。
// 全行復元後の高さは、最初から新幅で計測したテーブルと一致する。
TEST_F(TableEvictionDWriteTest, RestoreAfterWidthChangeCorrectsRowHeights)
{
    std::string md = "| A | B |\n|---|---|\n";
    for (int i = 0; i < 40; ++i) {
        md += "| long text that wraps when the column becomes narrow " + std::to_string(i) + " | another long text in the second column |\n";
    }
    auto pl = ParseAndLayout(md);
    const int idx = FindTableNode(pl.nodes);
    ASSERT_GE(idx, 0);
    auto& entry = pl.cache[idx];

    EvictWholeTable(pl.cache, idx);

    const float narrow = 200.0f;
    const MeasureViewportRange vp{ 0.0f, 60.0f };
    entry.layout_dirty = true;
    measurer_.MeasureNode(pl.nodes[idx], entry, narrow, nullptr, vp);
    EXPECT_FALSE(entry.layout_dirty);
    ASSERT_TRUE(entry.table_layout->HasEvictedRows());

    const auto result = measurer_.RestoreEvictedTableRows(pl.nodes[idx], entry, narrow, MeasureViewportRange{});
    EXPECT_TRUE(result.restored);
    EXPECT_TRUE(result.height_changed);
    EXPECT_FALSE(entry.table_layout->HasEvictedRows());

    auto fresh = ParseAndLayout(md);
    auto& fresh_entry = fresh.cache[idx];
    fresh_entry.layout_dirty = true;
    measurer_.MeasureNode(fresh.nodes[idx], fresh_entry, narrow);
    EXPECT_FLOAT_EQ(entry.height, fresh_entry.height);
}

// EnsureVisibleLayout は dirty でないテーブルの evict 行を可視分だけ復元する。
TEST_F(TableEvictionDWriteTest, EnsureVisibleLayoutRestoresEvictedVisibleRows)
{
    auto pl = ParseAndLayout(MakeBigTableMd(60));
    const int idx = FindTableNode(pl.nodes);
    ASSERT_GE(idx, 0);
    auto& entry = pl.cache[idx];
    auto& tl = *entry.table_layout;

    EvictWholeTable(pl.cache, idx);
    ASSERT_EQ(CountNonNullCells(tl), 0u);

    const uint32_t gen_before = pl.cache.GetEffectsGeneration();
    const bool updated = engine_.EnsureVisibleLayout(pl.nodes, pl.cache, 800.0f, pl.cache.Top(idx), pl.cache.Top(idx) + 80.0f);

    EXPECT_TRUE(updated);
    EXPECT_GT(CountNonNullCells(tl), 0u);
    EXPECT_NE(pl.cache.GetEffectsGeneration(), gen_before) << "復元セルへ effects を適用させるため世代を進める";
    EXPECT_FALSE(entry.layout_dirty);
}

// scheduler を渡すと巨大テーブルのセル生成が並列化されるが、結果は直列と一致する。
TEST_F(TableEvictionDWriteTest, ParallelCellBuildMatchesSerial)
{
    const auto md = MakeBigTableMd(400);
    auto serial = ParseAndLayout(md);

    TaskScheduler scheduler;
    scheduler.Init(3);
    measurer_.SetScheduler(&scheduler);
    auto parallel = ParseAndLayout(md);
    measurer_.SetScheduler(nullptr);
    scheduler.Shutdown();

    const int idx = FindTableNode(serial.nodes);
    ASSERT_GE(idx, 0);
    const auto& s = *serial.cache[idx].table_layout;
    const auto& p = *parallel.cache[idx].table_layout;
    EXPECT_EQ(CountNonNullCells(p), p.cell_layouts.size());
    EXPECT_EQ(s.natural_col_widths, p.natural_col_widths);
    EXPECT_EQ(s.col_widths, p.col_widths);
    EXPECT_FLOAT_EQ(serial.cache[idx].height, parallel.cache[idx].height);
}
