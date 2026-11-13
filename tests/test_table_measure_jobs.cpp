// 巨大な表の最初の計測をワーカーで行う TableMeasureJobs (性能レビュー A6) と、組み終えた表を置く
// LayoutService::PlaceMeasuredTables のテスト。
#include <gtest/gtest.h>
#include "document.h"
#include "document_test_helpers.h"
#include "dwrite_test_base.h"
#include "task_scheduler.h"
#include "viewport_manager.h"
#include <atomic>
#include <chrono>
#include <optional>
#include <string>
#include <string_view>

namespace {

constexpr float kWidth = 800.0f;
constexpr float kScreen = 600.0f;

// 6 列。rows 行で、2000 行なら 12000 セル (TableMeasureJobs::kMinCells 超)。前後に段落を置く。first_cell は 1 行目の最初のセル。
std::string TableMd(size_t rows, std::string_view first_cell = "r0")
{
    std::string md = "# 表\n\n前の段落。\n\n| A | B | C | D | E | F |\n|---|---|---|---|---|---|\n";
    for (size_t r = 0; r < rows; ++r) {
        const std::string first = r == 0 ? std::string(first_cell) : "r" + std::to_string(r);
        md += "| " + first + " | 値" + std::to_string(r % 13) + " | 少し長めの説明 " + std::to_string(r) + " | x | y | z |\n";
    }
    md += "\n後ろの段落。\n";
    return md;
}

class TableMeasureJobsTest : public DWriteTestBase {
protected:
    TaskScheduler scheduler_{ 4 };
    TableMeasureJobs jobs_{ scheduler_, measurer_ };
    ViewportManager viewport_;
    LayoutService service_{ engine_, viewport_ };
    std::optional<Document> doc_;
    LayoutCache cache_;
    size_t table_ = 0;
    std::atomic<int> wakes_{ 0 };

    void SetUp() override
    {
        DWriteTestBase::SetUp();
        measurer_.SetScheduler(&scheduler_);
        engine_.SetTableJobs(&jobs_);
        jobs_.SetWake([this] { wakes_.fetch_add(1); });
    }
    void TearDown() override
    {
        jobs_.CancelAll();
        measurer_.SetScheduler(nullptr);
    }

    // md を開いたときと同じく、推定してから最初の画面を測る。
    void Open(std::string md)
    {
        doc_.emplace(Document::FromMarkdown(std::move(md), L"t.md"));
        jobs_.SetDocument(doc_->ShareData());
        cache_ = layout::MakeEstimatedLayoutCache(doc_->GetNodes(), theme_, theme_.ContentWidth(kWidth));
        table_ = static_cast<size_t>(FindFirstNodeIndexByType(doc_->GetNodes(), NodeType::Table));
        service_.ViewportLayout(*doc_, cache_, kWidth, kScreen);
    }

    // 組み終えた表を待って受け取る。
    std::vector<TableMeasureJobs::Result> WaitResults()
    {
        std::vector<TableMeasureJobs::Result> results;
        PollUntil([&] { results = jobs_.TakeResults(); return !results.empty(); }, std::chrono::seconds(30));
        return results;
    }

    float NodeWidth(float width) const
    {
        return layout::NodeContentWidth(doc_->GetNodes()[table_], theme_, theme_.ContentWidth(width));
    }

    // 同じ表を UI スレッドで測ったときの高さ。
    float SyncHeight() const
    {
        NodeLayoutEntry entry;
        MeasureEntry(measurer_, doc_->GetNodes()[table_], entry, NodeWidth(kWidth), nullptr);
        return entry.height;
    }
};

} // namespace

// 巨大な表は見えていても測らずに dirty のまま残し、ワーカーで組んだ結果を置くと UI スレッドで測ったときと同じ高さになる。
TEST_F(TableMeasureJobsTest, DefersGiantTableAndPlacesSameGeometry)
{
    Open(TableMd(2000));
    ASSERT_TRUE(cache_[table_].layout_dirty);
    ASSERT_EQ(cache_[table_].table_layout(), nullptr);
    // 前の段落は測ってある。
    EXPECT_FALSE(cache_[0].layout_dirty);

    auto results = WaitResults();
    ASSERT_EQ(results.size(), 1u);
    EXPECT_EQ(results[0].node, table_);
    const auto placed = service_.PlaceMeasuredTables(*doc_, cache_, kWidth, std::move(results));
    EXPECT_TRUE(placed.placed);
    EXPECT_FALSE(placed.stale_inline);

    const auto& entry = cache_[table_];
    EXPECT_FALSE(entry.layout_dirty);
    ASSERT_NE(entry.table_layout(), nullptr);
    EXPECT_FLOAT_EQ(entry.height, SyncHeight());
    // 後ろの段落の位置も組み直してある (全部を組み直しても動かない)。
    const float after = cache_.Top(table_ + 1);
    layout::RecomputeYPositions(doc_->GetNodes(), cache_, theme_);
    EXPECT_FLOAT_EQ(cache_.Top(table_ + 1), after);
    EXPECT_GT(after, cache_.Top(table_) + cache_[table_].height);
}

// セル数が kMinCells 以下の表は、これまでどおりその場で測る。
TEST_F(TableMeasureJobsTest, SmallTableIsMeasuredInPlace)
{
    Open(TableMd(100));
    EXPECT_FALSE(cache_[table_].layout_dirty);
    EXPECT_NE(cache_[table_].table_layout(), nullptr);
}

// 組んでいる間に計測が何度通っても、組み始めるのは 1 回だけ。
TEST_F(TableMeasureJobsTest, DoesNotRestartWhileRunning)
{
    Open(TableMd(2000));
    for (int i = 0; i < 5; ++i) {
        engine_.EnsureVisibleLayout(doc_->GetNodes(), cache_, kWidth, 0.0f, kScreen);
    }
    EXPECT_EQ(WaitResults().size(), 1u);
    jobs_.CancelAll();
    EXPECT_EQ(wakes_.load(), 1);
}

// 文書を差し替えたら、前の文書で組み終えていた結果も捨てる (ノードの番号が別の文書のものになる)。
TEST_F(TableMeasureJobsTest, DropsResultsOfReplacedDocument)
{
    Open(TableMd(2000));
    // 受け取る前に組み終えるのを待つ。
    PollUntil([&] { return wakes_.load() > 0; }, std::chrono::seconds(30));
    ASSERT_EQ(wakes_.load(), 1);
    const Document other = Document::FromMarkdown("別の文書\n", L"other.md");
    jobs_.SetDocument(other.ShareData());
    EXPECT_TRUE(jobs_.TakeResults().empty());
}

// CancelAll は組みが抜けるまで待ち、結果を捨てる。止めた表は次の計測で組み直す。
TEST_F(TableMeasureJobsTest, CancelAllDropsAndAllowsRestart)
{
    Open(TableMd(2000));
    jobs_.CancelAll();
    EXPECT_TRUE(jobs_.TakeResults().empty());
    ASSERT_TRUE(cache_[table_].layout_dirty);

    engine_.EnsureVisibleLayout(doc_->GetNodes(), cache_, kWidth, 0.0f, kScreen);
    EXPECT_EQ(WaitResults().size(), 1u);
}

// 止める印が立っていたら、組まずに table が null のものを返す。
TEST_F(TableMeasureJobsTest, BuildTableStopsWhenRequested)
{
    const Document doc = Document::FromMarkdown(TableMd(2000), L"t.md");
    const auto& node = doc.GetNodes()[static_cast<size_t>(FindFirstNodeIndexByType(doc.GetNodes(), NodeType::Table))];
    std::stop_source stop;
    stop.request_stop();
    EXPECT_EQ(measurer_.BuildTable(node, 700.0f, stop.get_token()).table, nullptr);
    // 止めなければ組む。
    const auto built = measurer_.BuildTable(node, 700.0f, {});
    EXPECT_NE(built.table, nullptr);
    EXPECT_GT(built.height, 0.0f);
}

// 推定の高さのまま表の中を見ていたら、置いた後も表の中の同じ割合の位置を見せる。
TEST_F(TableMeasureJobsTest, KeepsRelativePositionInsidePlacedTable)
{
    Open(TableMd(2000));
    const float estimated = cache_[table_].height;
    viewport_.SetScrollTarget(static_cast<int>(table_), estimated * 0.5f);
    viewport_.ApplyScrollTarget(cache_);

    auto results = WaitResults();
    ASSERT_EQ(results.size(), 1u);
    service_.PlaceMeasuredTables(*doc_, cache_, kWidth, std::move(results));

    const float measured = cache_[table_].height;
    ASSERT_NE(measured, estimated);
    EXPECT_NEAR(viewport_.GetScrollY(), cache_.Top(table_) + measured * 0.5f, 1.0f);
}

// 組む間に幅が変わったら、表は置くが dirty のまま残し、次の計測で今の幅に組み直させる。
TEST_F(TableMeasureJobsTest, WidthChangedDuringJobLeavesDirty)
{
    Open(TableMd(2000));
    auto results = WaitResults();
    ASSERT_EQ(results.size(), 1u);
    constexpr float kNarrow = 600.0f;
    service_.PlaceMeasuredTables(*doc_, cache_, kNarrow, std::move(results));
    EXPECT_TRUE(cache_[table_].layout_dirty);
    ASSERT_NE(cache_[table_].table_layout(), nullptr);

    // 幾何があるので、ワーカーに回さず列幅だけ組み直してその場で確定する。
    engine_.Reestimate(doc_->GetNodes(), cache_, kNarrow);
    const float top = cache_.Top(table_);
    engine_.EnsureVisibleLayout(doc_->GetNodes(), cache_, kNarrow, top, top + kScreen);
    EXPECT_FALSE(cache_[table_].layout_dirty);
    EXPECT_TRUE(cache_[table_].table_layout()->GeometryValidAt(NodeWidth(kNarrow)));
}

// 組む間に数式か画像の寸法が届いても、どちらも含まない表には影響しない。
TEST_F(TableMeasureJobsTest, InlineChangeIgnoresTableWithoutInlineContent)
{
    Open(TableMd(2000));
    jobs_.NoteInlineChanged();
    auto results = WaitResults();
    ASSERT_EQ(results.size(), 1u);
    EXPECT_TRUE(results[0].inline_changed);
    const auto placed = service_.PlaceMeasuredTables(*doc_, cache_, kWidth, std::move(results));
    EXPECT_FALSE(placed.stale_inline);
    EXPECT_FALSE(cache_[table_].pending_inline);
}

// 画像を含む表は、組む間に寸法が届いたら仮に組んだものとして置き、組み直させる。
TEST_F(TableMeasureJobsTest, InlineChangeMarksTableWithImageAsPending)
{
    Open(TableMd(2000, "![icon](icon.png)"));
    ASSERT_TRUE(doc_->GetNodes()[table_].has_images());
    jobs_.NoteInlineChanged();
    auto results = WaitResults();
    ASSERT_EQ(results.size(), 1u);
    const auto placed = service_.PlaceMeasuredTables(*doc_, cache_, kWidth, std::move(results));
    EXPECT_TRUE(placed.stale_inline);
    EXPECT_TRUE(cache_[table_].pending_inline);
}
