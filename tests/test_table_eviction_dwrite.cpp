// issue#210: スクロールでテーブルセル描画が欠落する不具合の再発防止テスト。
// EvictInvisibleTableRows で行単位 evict された行が、dirty を経由せず
// RestoreEvictedTableRows / EnsureVisibleLayout で可視行だけ復元される一連のフローを検証する。
#include <gtest/gtest.h>
#include "document_test_helpers.h"
#include "dwrite_test_base.h"
#include "task_scheduler.h"
#include <algorithm>
#include <array>
#include <string>

namespace {

class TableEvictionDWriteTest : public DWriteTestBase {
protected:
    ParsedLayout pl_;
    int idx_ = -1; // pl_ の最初の表

    // 3 列。セル layout を残さない大きな表 (kMaxKeptTableCells = 1024 セル超) にするには 342 行以上。
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

    static size_t CountNonNullCells(const TableLayoutData& tl)
    {
        return static_cast<size_t>(std::ranges::count_if(std::views::iota(size_t{ 0 }, tl.CellCount()), [&](size_t ci) { return tl.CellLayout(ci) != nullptr; }));
    }

    // md を組んで最初の表を対象にする。表の幾何が作られていることも確かめる。
    void LayoutTable(std::string_view md, float viewport_top = -YRange::kInf, float viewport_bottom = YRange::kInf)
    {
        pl_ = ParseAndLayout(md, 800.0f, viewport_top, viewport_bottom);
        idx_ = FindFirstNodeIndexByType(pl_.nodes, NodeType::Table);
        ASSERT_GE(idx_, 0);
        ASSERT_NE(Entry().table_layout(), nullptr);
    }

    NodeLayoutEntry& Entry()
    {
        return pl_.cache[idx_];
    }

    const Node& TableNode() const
    {
        return pl_.nodes[idx_];
    }

    // テーブル全体を一旦 evict (viewport より遠い範囲) → 全セルが null になる。
    void EvictWholeTable()
    {
        const float far_top = pl_.cache.Top(idx_) - 10000.0f;
        pl_.cache.EvictInvisibleTableRows(std::array{ static_cast<size_t>(idx_) }, YRange{ far_top, far_top + 1000.0f });
    }

    TableRestoreResult Restore(YRange viewport)
    {
        return RestoreEntryTableRows(measurer_, TableNode(), Entry(), viewport);
    }

    // 書き込み先の entry を dirty にして、表を計測し直す。
    void Measure(NodeLayoutEntry& entry, float width)
    {
        entry.layout_dirty = true;
        MeasureEntry(measurer_, TableNode(), entry, width, nullptr);
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
    ASSERT_NO_FATAL_FAILURE(LayoutTable(MakeBigTableMd(40)));
    auto& entry = Entry();
    auto& tl = *entry.table_layout();

    const size_t total_cells = tl.CellCount();
    ASSERT_GT(total_cells, 0u);
    ASSERT_EQ(CountNonNullCells(tl), total_cells);
    ASSERT_FALSE(entry.layout_dirty);

    pl_.cache.EvictInvisibleTableRows(std::array{ static_cast<size_t>(idx_) }, YRange{ pl_.cache.Top(idx_), pl_.cache.Top(idx_) + 80.0f });

    EXPECT_TRUE(tl.HasEvictedRows());
    EXPECT_FALSE(entry.layout_dirty);
    EXPECT_LT(CountNonNullCells(tl), total_cells)
        << "範囲外行のセルが Reset されているはず";
}

// 全行が viewport 内なら evict は発生しない。
TEST_F(TableEvictionDWriteTest, EvictInvisibleRowsNoOpWhenAllVisible)
{
    ASSERT_NO_FATAL_FAILURE(LayoutTable(MakeBigTableMd(5)));
    auto& entry = Entry();
    auto& tl = *entry.table_layout();

    const size_t total_cells = tl.CellCount();
    pl_.cache.EvictInvisibleTableRows(std::array{ static_cast<size_t>(idx_) }, YRange{ pl_.cache.Top(idx_) - 150.0f, pl_.cache.Bottom(idx_) + 150.0f });

    EXPECT_FALSE(tl.HasEvictedRows());
    EXPECT_FALSE(entry.layout_dirty);
    EXPECT_EQ(CountNonNullCells(tl), total_cells);
}

// 同じ幅での MeasureNode は幾何が不変なので超高速パスで抜け、全行を再計測しない。
TEST_F(TableEvictionDWriteTest, SameWidthMeasureTakesFastPathAfterEviction)
{
    ASSERT_NO_FATAL_FAILURE(LayoutTable(MakeBigTableMd(40)));
    auto& entry = Entry();
    auto& tl = *entry.table_layout();
    const float height_before = entry.height;

    EvictWholeTable();
    ASSERT_EQ(CountNonNullCells(tl), 0u);

    Measure(entry, ContentWidth());

    EXPECT_FALSE(entry.layout_dirty);
    EXPECT_EQ(CountNonNullCells(tl), 0u) << "幅不変なら MeasureNode はセルを作り直さない";
    EXPECT_TRUE(tl.HasEvictedRows());
    EXPECT_FLOAT_EQ(entry.height, height_before);
}

// 別の幅で推定が entry.height を伸ばした後に元の幅へ戻すと、表は前の幾何のまま測り直さずに済むが、
// 高さは伸びた値ではなく幾何の高さに戻す (issue #158: entry.height と行の高さの合計を食い違わせない)。
TEST_F(TableEvictionDWriteTest, HeightReturnsToGeometryAfterEstimateGrowth)
{
    ASSERT_NO_FATAL_FAILURE(LayoutTable("# head\n\n" + MakeBigTableMd(40)));
    const float measured = Entry().height;
    ASSERT_FLOAT_EQ(measured, Entry().table_layout()->TotalHeight());

    // 表を viewport の外 (先頭余白の中だけを見る) に置いたまま別の幅で推定させ、推定が実測より大きく伸ばした状態にする。
    ComputeLayout(engine_, pl_.nodes, pl_.cache, 600.0f, 0.0f, theme_.margin_top * 0.5f);
    Entry().height = measured + 100.0f;

    ComputeLayout(engine_, pl_.nodes, pl_.cache, 800.0f);
    EXPECT_FLOAT_EQ(Entry().height, measured);
}

// RestoreEvictedTableRows は viewport 内の evict 行だけ復元し、範囲外は null のまま残す。
TEST_F(TableEvictionDWriteTest, RestoreRestoresOnlyVisibleRows)
{
    ASSERT_NO_FATAL_FAILURE(LayoutTable(MakeBigTableMd(60)));
    auto& entry = Entry();
    auto& tl = *entry.table_layout();
    const size_t total_cells = tl.CellCount();
    const float height_before = entry.height;

    EvictWholeTable();
    const size_t after_evict = CountNonNullCells(tl);
    ASSERT_LT(after_evict, total_cells);

    const YRange vp{ 0.0f, 80.0f };
    const auto result = Restore(vp);

    EXPECT_TRUE(result.restored);
    EXPECT_FALSE(result.height.has_value());
    const size_t after_restore = CountNonNullCells(tl);
    EXPECT_GT(after_restore, after_evict);
    EXPECT_LT(after_restore, total_cells);
    EXPECT_TRUE(tl.HasEvictedRows());
    EXPECT_FALSE(entry.layout_dirty);
    EXPECT_FLOAT_EQ(entry.height, height_before);
}

// 検索ハイライトは evict 中のセルのマッチを未計算のまま残すので、行を復元しても計算済みの矩形は捨てない。
// スクロールで行が復元されるたびに表全体のマッチを計算し直すのを防ぐ。
// 部分復元 → 別 viewport で追加復元 → 最終的に全セルが揃い evict 行がなくなる。
TEST_F(TableEvictionDWriteTest, ProgressivelyRestoresAcrossScrolls)
{
    ASSERT_NO_FATAL_FAILURE(LayoutTable(MakeBigTableMd(60)));
    auto& entry = Entry();
    auto& tl = *entry.table_layout();
    const size_t total_cells = tl.CellCount();

    EvictWholeTable();
    ASSERT_LT(CountNonNullCells(tl), total_cells);

    const YRange vp1{ 0.0f, entry.height * 0.5f };
    Restore(vp1);
    EXPECT_TRUE(tl.HasEvictedRows());

    const YRange vp2{ 0.0f, entry.height };
    Restore(vp2);

    EXPECT_EQ(CountNonNullCells(tl), total_cells);
    EXPECT_FALSE(tl.HasEvictedRows());
}

// 2 回目の evict は前回の生存範囲との差分だけを対象にし、復元済み行は再び evict できる。
TEST_F(TableEvictionDWriteTest, EvictIsDifferentialAgainstLiveRange)
{
    ASSERT_NO_FATAL_FAILURE(LayoutTable(MakeBigTableMd(60)));
    auto& entry = Entry();
    auto& tl = *entry.table_layout();

    EvictWholeTable();
    EXPECT_TRUE(tl.LiveRows().empty()) << "全行 evict 後は生存範囲が空";

    const YRange vp{ 0.0f, 80.0f };
    Restore(vp);
    const size_t restored_cells = CountNonNullCells(tl);
    ASSERT_GT(restored_cells, 0u);
    EXPECT_FALSE(tl.LiveRows().empty());

    EvictWholeTable();
    EXPECT_EQ(CountNonNullCells(tl), 0u) << "復元済み行は生存範囲に入り、再 evict される";
}

// ノード単位 evict でもテーブルは列幅・行高さを残し、同じ幅なら再計測不要で戻れる。
TEST_F(TableEvictionDWriteTest, NodeEvictionKeepsTableGeometry)
{
    ASSERT_NO_FATAL_FAILURE(LayoutTable("para\n\n" + MakeBigTableMd(40)));
    auto& entry = Entry();
    const float height_before = entry.height;
    const auto col_widths_before = entry.table_layout()->col_widths;

    // keep 範囲を先頭ノードだけにしてテーブルを evict 対象にする。
    pl_.cache.EvictTextLayouts(0, 1);

    ASSERT_NE(entry.table_layout(), nullptr) << "幾何ごと破棄すると再表示時に全セル再構築になる";
    auto& tl = *entry.table_layout();
    EXPECT_EQ(CountNonNullCells(tl), 0u);
    EXPECT_TRUE(tl.HasEvictedRows());
    EXPECT_EQ(tl.col_widths, col_widths_before);
    EXPECT_FALSE(entry.layout_dirty);

    const YRange vp{ 0.0f, entry.height };
    const auto result = Restore(vp);
    EXPECT_TRUE(result.restored);
    EXPECT_EQ(CountNonNullCells(tl), tl.CellCount());
    EXPECT_FLOAT_EQ(entry.height, height_before);
}

// 幅変更で evict 中だった行は折り返しの行数を見積もった高さになり、復元時に実測へ揃う。
// 全行復元後の高さは、最初から新幅で計測したテーブルと一致する。
TEST_F(TableEvictionDWriteTest, RestoreAfterWidthChangeCorrectsRowHeights)
{
    std::string md = "| A | B |\n|---|---|\n";
    for (int i = 0; i < 40; ++i) {
        md += "| long text that wraps when the column becomes narrow " + std::to_string(i) + " | another long text in the second column |\n";
    }
    ASSERT_NO_FATAL_FAILURE(LayoutTable(md));
    auto& entry = Entry();

    EvictWholeTable();

    const float narrow = 200.0f;
    Measure(entry, narrow);
    EXPECT_FALSE(entry.layout_dirty);
    ASSERT_TRUE(entry.table_layout()->HasEvictedRows());

    const auto result = Restore(YRange::All());
    EXPECT_TRUE(result.restored);
    EXPECT_FALSE(entry.table_layout()->HasEvictedRows());

    auto fresh = ParseAndLayout(md);
    auto& fresh_entry = fresh.cache[idx_];
    fresh_entry.layout_dirty = true;
    MeasureEntry(measurer_, fresh.nodes[idx_], fresh_entry, narrow, nullptr);
    EXPECT_FLOAT_EQ(entry.height, fresh_entry.height);
}

// EnsureVisibleLayout は dirty でないテーブルの evict 行を可視分だけ復元する。
TEST_F(TableEvictionDWriteTest, EnsureVisibleLayoutRestoresEvictedVisibleRows)
{
    ASSERT_NO_FATAL_FAILURE(LayoutTable(MakeBigTableMd(60)));
    auto& entry = Entry();
    auto& tl = *entry.table_layout();

    EvictWholeTable();
    ASSERT_EQ(CountNonNullCells(tl), 0u);

    const bool updated = engine_.EnsureVisibleLayout(pl_.nodes, pl_.cache, 800.0f, pl_.cache.Top(idx_), pl_.cache.Top(idx_) + 80.0f);

    EXPECT_TRUE(updated);
    EXPECT_GT(CountNonNullCells(tl), 0u);
    EXPECT_FALSE(entry.layout_dirty);
}

// 表だけの文書でも、見えている範囲の外に退避した行が残れば先読みを残し、先読みが範囲の行を戻す。
TEST_F(TableEvictionDWriteTest, VisibleTableWithEvictedRowsKeepsPrefetchPending)
{
    ASSERT_NO_FATAL_FAILURE(LayoutTable(MakeBigTableMd(60)));
    ASSERT_EQ(pl_.nodes.size(), 1u);
    auto& tl = *Entry().table_layout();
    EvictWholeTable();

    const float top = pl_.cache.Top(idx_);
    ASSERT_TRUE(engine_.EnsureVisibleLayout(pl_.nodes, pl_.cache, 800.0f, top, top + 80.0f));
    EXPECT_TRUE(pl_.cache.HasDirtyNodes());

    const size_t visible_cells = CountNonNullCells(tl);
    const auto batch = engine_.ProcessDirtyBatch(pl_.nodes, pl_.cache, 800.0f, kNoLayoutDeadline, YRange{ top, top + 400.0f });
    EXPECT_TRUE(batch.updated);
    EXPECT_FALSE(batch.remaining);
    EXPECT_FALSE(pl_.cache.HasDirtyNodes());
    EXPECT_GT(CountNonNullCells(tl), visible_cells);
}

// 幅の変更で組み直した表の見えている evict 行は、同じ EnsureVisibleLayout の次のパスで戻る。
TEST_F(TableEvictionDWriteTest, WidthChangeRestoresVisibleRowsInNextPass)
{
    ASSERT_NO_FATAL_FAILURE(LayoutTable(MakeBigTableMd(60)));
    EvictWholeTable();

    ComputeLayout(engine_, pl_.nodes, pl_.cache, 600.0f, pl_.cache.Top(idx_), pl_.cache.Top(idx_) + 80.0f);

    const auto& tl = *Entry().table_layout();
    EXPECT_TRUE(tl.GeometryValidAt(layout::NodeContentWidth(TableNode(), theme_, theme_.ContentWidth(600.0f))));
    EXPECT_NE(tl.GetCellLayout(0, 0), nullptr);
    EXPECT_TRUE(tl.HasEvictedRows());
}

// 再計測のしきい値に届かない幅の変更では表は dirty にならず、幾何は前の幅のまま残る。見えた退避行はその幾何で戻す。
// 戻さないと、次にしきい値を超える幅の変更があるまでセルの文字が空になる。
TEST_F(TableEvictionDWriteTest, RestoresVisibleRowsAfterWidthChangeBelowThreshold)
{
    ASSERT_NO_FATAL_FAILURE(LayoutTable(MakeBigTableMd(350), 0.0f, 600.0f));
    const auto& tl = *Entry().table_layout();
    constexpr float kNudgedWidth = 801.0f;
    ASSERT_FALSE(engine_.Reestimate(pl_.nodes, pl_.cache, kNudgedWidth));
    ASSERT_FALSE(Entry().layout_dirty);

    const float mid = pl_.cache.Top(idx_) + Entry().height * 0.5f;
    engine_.EnsureVisibleLayout(pl_.nodes, pl_.cache, kNudgedWidth, mid, mid + 100.0f);

    const YRange local = YRange{ mid, mid + 100.0f }.ToLocal(pl_.cache.Top(idx_));
    const auto [r_begin, r_end] = tl.VisibleRowRange(local.top, local.bottom);
    ASSERT_LT(r_begin, r_end);
    for (size_t r = r_begin; r < r_end; ++r) {
        EXPECT_NE(tl.GetCellLayout(r, 0), nullptr) << "row " << r;
    }
}

// scheduler を渡すと巨大テーブルのセル生成が並列化されるが、結果は直列と一致する。
TEST_F(TableEvictionDWriteTest, ParallelCellBuildMatchesSerial)
{
    const auto md = MakeBigTableMd(400);
    auto serial = ParseAndLayout(md);

    TaskScheduler scheduler{ 3 };
    measurer_.SetScheduler(&scheduler);
    auto parallel = ParseAndLayout(md);
    measurer_.SetScheduler(nullptr);
    scheduler.Shutdown();

    const int idx = FindFirstNodeIndexByType(serial.nodes, NodeType::Table);
    ASSERT_GE(idx, 0);
    const auto& s = *serial.cache[idx].table_layout();
    const auto& p = *parallel.cache[idx].table_layout();
    EXPECT_EQ(CountNonNullCells(p), p.CellCount());
    EXPECT_EQ(s.natural_col_widths, p.natural_col_widths);
    EXPECT_EQ(s.min_col_widths, p.min_col_widths);
    EXPECT_EQ(s.col_widths, p.col_widths);
    EXPECT_FLOAT_EQ(serial.cache[idx].height, parallel.cache[idx].height);
}

// scheduler があると evict 行の復元も行チャンクで並列化されるが、復元結果と行高さは直列と一致する。
TEST_F(TableEvictionDWriteTest, ParallelRestoreMatchesSerial)
{
    std::string md = "| A | B |\n|---|---|\n";
    for (int i = 0; i < 100; ++i) {
        md += "| long text that wraps when the column becomes narrow " + std::to_string(i) + " | another long text in the second column |\n";
    }
    const float narrow = 200.0f;
    const auto restore = [&](TaskScheduler* scheduler) {
        LayoutTable(md);
        auto& entry = Entry();
        EvictWholeTable();
        // 狭い幅で行高さを見積もりのまま残し、復元で実測に揃わせる。
        Measure(entry, narrow);
        measurer_.SetScheduler(scheduler);
        const auto result = Restore(YRange::All());
        measurer_.SetScheduler(nullptr);
        EXPECT_TRUE(result.restored);
        EXPECT_FALSE(entry.table_layout()->HasEvictedRows());
        EXPECT_EQ(CountNonNullCells(*entry.table_layout()), entry.table_layout()->CellCount());
        return std::pair{ entry.table_layout()->row_heights, entry.height };
    };

    const auto serial = restore(nullptr);
    TaskScheduler scheduler{ 3 };
    const auto parallel = restore(&scheduler);
    scheduler.Shutdown();

    EXPECT_EQ(serial.first, parallel.first);
    EXPECT_FLOAT_EQ(serial.second, parallel.second);
}

// 捨てたセルや本文のレイアウトは、UI スレッドで解放せずに TakeReleasedLayouts で渡す。
TEST_F(TableEvictionDWriteTest, EvictedLayoutsAreHandedOverForRelease)
{
    ASSERT_NO_FATAL_FAILURE(LayoutTable("para\n\n" + MakeBigTableMd(40)));
    auto& tl = *Entry().table_layout();
    const size_t before = CountNonNullCells(tl);
    ASSERT_TRUE(pl_.cache.TakeReleasedLayouts().empty());

    EvictWholeTable();
    EXPECT_EQ(pl_.cache.TakeReleasedLayouts().size(), before);
    EXPECT_EQ(CountNonNullCells(tl), 0u);

    ASSERT_NE(pl_.cache[0].text_layout(), nullptr);
    pl_.cache.InvalidateAllLayouts(pl_.nodes);
    EXPECT_EQ(pl_.cache.TakeReleasedLayouts().size(), 1u) << "段落の text_layout だけが残っていた";
    EXPECT_EQ(pl_.cache[0].text_layout(), nullptr);
    EXPECT_EQ(Entry().table_layout(), nullptr) << "旧倍率の表の幾何を残さない";
}

// 大きな表の初回計測はセル layout を残さず、全行を evict 済みにする (5 万行で 2GB になるのを防ぐ)。
// 折り返さない表なら、行の高さは全セルを作ったときと同じになる。
TEST_F(TableEvictionDWriteTest, LargeTableFirstMeasureKeepsNoCellLayouts)
{
    ASSERT_NO_FATAL_FAILURE(LayoutTable(MakeBigTableMd(350)));
    const auto& built = *Entry().table_layout();
    const auto row_heights_built = built.row_heights;
    const float height_built = Entry().height;
    ASSERT_EQ(CountNonNullCells(built), built.CellCount());

    NodeLayoutEntry entry;
    Measure(entry, ContentWidth());

    const auto& tl = *entry.table_layout();
    EXPECT_FALSE(entry.layout_dirty);
    EXPECT_EQ(CountNonNullCells(tl), 0u);
    for (size_t r = 0; r < tl.row_count; ++r) {
        EXPECT_TRUE(tl.IsRowEvicted(r)) << r;
    }
    EXPECT_EQ(tl.row_heights, row_heights_built);
    EXPECT_FLOAT_EQ(entry.height, height_built);
}

// 大きな表を可視範囲だけ計測すると、セル layout は可視行の分だけ作られる。
TEST_F(TableEvictionDWriteTest, LargeTableBuildsOnlyVisibleRows)
{
    ASSERT_NO_FATAL_FAILURE(LayoutTable(MakeBigTableMd(350), 0.0f, 600.0f));
    const auto& tl = *Entry().table_layout();
    const size_t built = CountNonNullCells(tl);

    EXPECT_GT(built, 0u);
    EXPECT_LT(built, tl.CellCount() / 10);
    EXPECT_FALSE(Entry().layout_dirty);
}

// 大きな表の初回計測は、列の中で中身 (テキストと装飾) が同じセルを 1 回だけ測り、他は代表の値を使う。
// 装飾が違えば幅も違うので別に測る。代表の値で見積もった行の高さは、全セルを作って測った高さと一致する。
TEST_F(TableEvictionDWriteTest, LargeTableMeasuresDuplicateCellsOnce)
{
    std::string md = "| A | B | C |\n|---|---|---|\n";
    for (size_t i = 0; i < 350; ++i) {
        md += (i % 3 == 0) ? "| **ok** " : "| ok ";
        md += "| r" + std::to_string(i) + " | same text in every row |\n";
    }
    ASSERT_NO_FATAL_FAILURE(LayoutTable(md));
    const auto& built = *Entry().table_layout();
    ASSERT_EQ(CountNonNullCells(built), built.CellCount());

    NodeLayoutEntry entry;
    Measure(entry, ContentWidth());
    const auto& tl = *entry.table_layout();

    // 0 行目は見出し。1 行目 (i=0) が太字、2 行目 (i=1) が普通の "ok"。
    const auto width = [&](size_t r, size_t c) { return tl.cell_natural_widths[tl.CellIndex(r, c)]; };
    EXPECT_GT(width(1, 0), width(2, 0));
    EXPECT_EQ(CountNonNullCells(tl), 0u);
    // built は全行を作り直して実測した高さ。
    EXPECT_EQ(tl.row_heights, built.row_heights);
}
