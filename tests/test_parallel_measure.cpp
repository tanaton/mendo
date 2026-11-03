#include <gtest/gtest.h>
#include <algorithm>
#include <atomic>
#include <initializer_list>
#include <numeric>
#include <ranges>
#include <stdexcept>
#include <string>
#include "document_test_helpers.h"
#include "layout.h"
#include "layout_invariants.h"
#include "mock_text_measurer.h"
#include "parser.h"
#include "task_scheduler.h"
#include "test_helpers.h"
#include "test_nodes.h"
#include "theme.h"

using layout::EstimateNodeHeights;

namespace {

// ProcessDirtyBatch のテスト用。ノード i は Paragraph で text_top = i * 100、height = 80。
struct DirtyNodeFixture {
    std::vector<Node> nodes;
    LayoutCache cache;

    void Build(size_t n, bool all_dirty)
    {
        for (size_t i = 0; i < n; ++i) {
            nodes.push_back(MakeTextNode("x"));
        }
        cache = LayoutCache(n);
        for (size_t i = 0; i < n; ++i) {
            cache.SetTop(i, static_cast<float>(i) * 100.0f);
            cache[i].height = 80.0f;
            cache[i].layout_dirty = all_dirty;
        }
    }

    void Build(size_t n, std::initializer_list<size_t> dirty_indices)
    {
        Build(n, false);
        for (size_t idx : dirty_indices) {
            cache[idx].layout_dirty = true;
        }
    }
};

class ParallelMeasureTest : public MockLayoutTestBase {
protected:
    TaskScheduler task_scheduler_{ 2 };
};

// scheduler なし (呼び出しスレッドで直列計測) の ProcessDirtyBatch 用。
class DirtyBatchSerialTest : public MockLayoutTestBase {};

class ThrowingMeasurer : public MockTextMeasurer {
public:
    const Node* throw_on = nullptr;
    // throw_on の計測を失敗させる。worker から呼ばれるので試行回数は atomic。
    // 例外の代わりに、書式やレイアウトを作れなかったときと同じく nullopt で失敗させる。
    bool fail_with_nullopt = false;
    mutable std::atomic<int> attempts{ 0 };

    std::optional<float> MeasureNode(const Node& node, NodeLayoutEntry& entry, float max_width) const override
    {
        if (&node == throw_on) {
            attempts.fetch_add(1);
            if (fail_with_nullopt) {
                return std::nullopt;
            }
            throw std::runtime_error("measure failed");
        }
        return MockTextMeasurer::MeasureNode(node, entry, max_width);
    }
};

// 計測に失敗するノードを指定できる計測器で、engine_ が MakeParagraphs の文書を組む。
class ThrowingMeasureTest : public ParallelMeasureTest {
protected:
    ThrowingMeasurer measurer_;

    void SetUp() override
    {
        ParallelMeasureTest::SetUp();
        ASSERT_TRUE(engine_.Init(&measurer_, theme_));
    }

    ParsedLayout LayoutParagraphs(int count)
    {
        return ParseAndLayout(MakeParagraphs(count));
    }
};

struct BatchRun {
    int processed = 0;
    bool more = false;
};

// f に ProcessDirtyBatch を 1 回かけ、計測したノード数と戻り値を返す。
BatchRun RunBatch(DirtyNodeFixture& f, const Theme& theme, MockTextMeasurer& measurer, YRange range, LayoutDeadline deadline, TaskScheduler* scheduler)
{
    LayoutEngine engine;
    EXPECT_TRUE(engine.Init(&measurer, theme));
    measurer.SetScheduler(scheduler);
    const size_t before = CountDirty(f.cache);
    const bool more = engine.ProcessDirtyBatch(f.nodes, f.cache, 800.0f, deadline, range);
    measurer.SetScheduler(nullptr);
    return { static_cast<int>(before - CountDirty(f.cache)), more };
}

// ProcessDirtyBatch を残り dirty なしまで回し、回したバッチ数を返す。収束しなければ失敗にする。
int DrainDirtyBatches(LayoutEngine& engine, const std::vector<Node>& nodes, LayoutCache& cache)
{
    constexpr int kMaxBatches = 20;
    for (int batches = 1; batches <= kMaxBatches; ++batches) {
        if (!engine.ProcessDirtyBatch(nodes, cache, 800.0f, kNoLayoutDeadline, YRange::All())) {
            return batches;
        }
    }
    ADD_FAILURE() << "ProcessDirtyBatch が収束しない";
    return kMaxBatches;
}

} // namespace

// 以下の 3 件は直列 (scheduler なし) と並列の両方で確かめる。
TEST_F(ParallelMeasureTest, NoDirtyMeasuresNothing)
{
    for (TaskScheduler* scheduler : { static_cast<TaskScheduler*>(nullptr), &task_scheduler_ }) {
        DirtyNodeFixture f;
        f.Build(10, false);
        const auto r = RunBatch(f, theme_, mock_, YRange::All(), kNoLayoutDeadline, scheduler);
        EXPECT_EQ(r.processed, 0);
        EXPECT_FALSE(r.more);
    }
}

// 期限を過ぎていても先頭の 1 ノードは計測し、残りは次の呼び出しに回す。
TEST_F(ParallelMeasureTest, ExpiredDeadlineMeasuresFirstNode)
{
    for (TaskScheduler* scheduler : { static_cast<TaskScheduler*>(nullptr), &task_scheduler_ }) {
        DirtyNodeFixture f;
        f.Build(100, true);
        const auto r = RunBatch(f, theme_, mock_, YRange::All(), LayoutDeadline{}, scheduler);
        EXPECT_EQ(r.processed, 1);
        EXPECT_TRUE(r.more);
        EXPECT_FALSE(f.cache[0].layout_dirty);
    }
}

// 範囲に重なる dirty だけを計測する。ノード i は [i * 100, i * 100 + 80]。上端がちょうど範囲の下端のノードは含む。
TEST_F(ParallelMeasureTest, ViewportClipMeasuresOnlyOverlappingNodes)
{
    struct Case {
        YRange range;
        size_t first;
        size_t last_plus_1;
    };
    const Case cases[] = {
        { layout::BufferedRange(150.0f, 200.0f, 0.0f), 1, 4 },
        { layout::BufferedRange(300.0f, 100.0f, 1.0f), 2, 6 },
        { layout::BufferedRange(200.0f, 400.0f, 1.0f), 0, 11 },
    };
    for (TaskScheduler* scheduler : { static_cast<TaskScheduler*>(nullptr), &task_scheduler_ }) {
        for (const auto& c : cases) {
            SCOPED_TRACE(c.first);
            DirtyNodeFixture f;
            f.Build(20, true);
            const auto r = RunBatch(f, theme_, mock_, c.range, kNoLayoutDeadline, scheduler);
            EXPECT_EQ(r.processed, static_cast<int>(c.last_plus_1 - c.first));
            EXPECT_FALSE(r.more);
            for (size_t i = 0; i < f.cache.size(); ++i) {
                EXPECT_EQ(f.cache[i].layout_dirty, i < c.first || i >= c.last_plus_1) << i;
            }
        }
    }
}

// 全 dirty を直列と並列で計測し、全ノードを計測し終えて height / layout_dirty / 計測数が一致する。
TEST_F(ParallelMeasureTest, MatchesSerialOutputAndClearsAllDirty)
{
    constexpr size_t N = 1000;
    DirtyNodeFixture f_serial;
    f_serial.Build(N, true);
    DirtyNodeFixture f_parallel;
    f_parallel.Build(N, true);

    const auto r_serial = RunBatch(f_serial, theme_, mock_, YRange::All(), kNoLayoutDeadline, nullptr);
    const auto r_parallel = RunBatch(f_parallel, theme_, mock_, YRange::All(), kNoLayoutDeadline, &task_scheduler_);

    EXPECT_EQ(r_parallel.processed, static_cast<int>(N));
    EXPECT_EQ(r_serial.processed, r_parallel.processed);
    EXPECT_EQ(r_serial.more, r_parallel.more);

    for (size_t i = 0; i < N; ++i) {
        EXPECT_FLOAT_EQ(f_serial.cache[i].height, f_parallel.cache[i].height) << "i=" << i;
        EXPECT_FALSE(f_parallel.cache[i].layout_dirty) << "i=" << i;
        EXPECT_FALSE(f_serial.cache[i].layout_dirty) << "i=" << i;
        EXPECT_GT(f_parallel.cache[i].height, 0.0f) << "i=" << i;
    }
}

// EnsureVisibleLayout も scheduler があれば可視 dirty を並列計測し、直列と同じ結果になる。
TEST_F(ParallelMeasureTest, EnsureVisibleLayoutParallelMatchesSerial)
{
    std::string md;
    for (int i = 0; i < 300; i++) {
        md += "Paragraph " + std::to_string(i) + " " + std::string(static_cast<size_t>(i % 50), 'w') + "\n\n";
    }
    const auto build = [&](LayoutEngine& engine, std::vector<Node>& nodes, LayoutCache& cache) {
        nodes = ParseNodes(md);
        cache = LayoutCache(nodes.size());
        EstimateNodeHeights(nodes, cache, theme_, 0.0f, {});
        engine.EnsureVisibleLayout(nodes, cache, 800.0f, 0.0f, 100000.0f);
    };

    LayoutEngine serial;
    ASSERT_TRUE(serial.Init(&mock_, theme_));
    std::vector<Node> s_nodes;
    LayoutCache s_cache;
    build(serial, s_nodes, s_cache);

    LayoutEngine parallel;
    ASSERT_TRUE(parallel.Init(&mock_, theme_));
    mock_.SetScheduler(&task_scheduler_);
    std::vector<Node> p_nodes;
    LayoutCache p_cache;
    build(parallel, p_nodes, p_cache);
    mock_.SetScheduler(nullptr);

    ASSERT_EQ(s_nodes.size(), p_nodes.size());
    for (size_t i = 0; i < s_nodes.size(); ++i) {
        EXPECT_FLOAT_EQ(s_cache[i].height, p_cache[i].height) << "i=" << i;
        EXPECT_FLOAT_EQ(s_cache.Top(i), p_cache.Top(i)) << "i=" << i;
        EXPECT_EQ(s_cache[i].layout_dirty, p_cache[i].layout_dirty) << "i=" << i;
    }
}

// 並列計測の chunk が例外で落ちても、落ちる前に計測できたノードの高さを Y に反映し、
// 計測できなかったノードは再試行に回さず dirty を外す。
TEST_F(ParallelMeasureTest, ProcessDirtyBatchGivesUpOnThrowingNode)
{
    struct Case {
        size_t dirty_count;
        size_t throw_at;
        bool clip;
    };
    constexpr size_t kFirstDirty = 5;
    const std::string md = MakeParagraphs(60);
    for (const Case c : { Case{ 10, 9, false }, Case{ 10, 9, true }, Case{ 10, 0, true }, Case{ 40, 20, false }, Case{ 40, 20, true } }) {
        SCOPED_TRACE("dirty=" + std::to_string(c.dirty_count) + " throw_at=" + std::to_string(c.throw_at) + " clip=" + std::to_string(c.clip));
        ThrowingMeasurer measurer;
        LayoutEngine engine;
        ASSERT_TRUE(engine.Init(&measurer, theme_));
        auto nodes = ParseNodes(md);
        LayoutCache cache(nodes.size());
        ComputeLayout(engine, nodes, cache, 800.0f);
        ASSERT_FALSE(cache.HasDirtyNodes());

        // 再計測で高さが変わるようにして、計測済みノードの Y 反映漏れを見えるようにする。
        measurer.line_height *= 1.5f;
        for (size_t k = 0; k < c.dirty_count; ++k) {
            cache[kFirstDirty + k].layout_dirty = true;
        }
        const size_t thrower = kFirstDirty + c.throw_at;
        measurer.throw_on = &nodes[thrower];

        measurer.SetScheduler(&task_scheduler_);
        const bool more = c.clip
            ? engine.ProcessDirtyBatch(nodes, cache, 800.0f, kNoLayoutDeadline, layout::BufferedRange(0.0f, 600.0f, 100.0f))
            : engine.ProcessDirtyBatch(nodes, cache, 800.0f, kNoLayoutDeadline, YRange::All());
        measurer.SetScheduler(nullptr);

        EXPECT_FALSE(cache[thrower].layout_dirty);
        EXPECT_FALSE(more);
        EXPECT_FALSE(cache.HasDirtyNodes()) << "計測できなかったノードでダーティ処理が続いている";
        EXPECT_TRUE(YChainConsistent(nodes, cache, theme_));
    }
}

// 例外を投げるノードは 1 回で諦めて dirty を外し、ダーティ処理を止める。
// 同じ chunk の他のノードは例外に巻き込まれず計測される。全レイアウトの無効化で再び試される。
TEST_F(ThrowingMeasureTest, ThrowingNodeIsNotRetried)
{
    constexpr size_t kFirstDirty = 5;
    constexpr size_t kDirtyCount = 10;
    constexpr size_t kThrower = kFirstDirty + 3;
    auto [nodes, cache] = LayoutParagraphs(60);

    measurer_.line_height *= 1.5f;
    for (size_t k = 0; k < kDirtyCount; ++k) {
        cache[kFirstDirty + k].layout_dirty = true;
    }
    measurer_.throw_on = &nodes[kThrower];

    measurer_.SetScheduler(&task_scheduler_);
    EXPECT_FALSE(engine_.ProcessDirtyBatch(nodes, cache, 800.0f, kNoLayoutDeadline, YRange::All()));
    for (size_t k = 0; k < kDirtyCount; ++k) {
        const size_t i = kFirstDirty + k;
        EXPECT_FALSE(cache[i].layout_dirty) << "i=" << i;
        if (i != kThrower) {
            EXPECT_TRUE(cache[i].is_measured()) << "i=" << i << " は例外を投げたノードと同じ chunk でも計測される";
        }
    }
    EXPECT_EQ(measurer_.attempts.load(), 1);
    EXPECT_FALSE(cache.HasDirtyNodes());
    EXPECT_TRUE(YChainConsistent(nodes, cache, theme_));

    cache.InvalidateAllLayouts(nodes);
    cache[kThrower].layout_dirty = true;
    DrainDirtyBatches(engine_, nodes, cache);
    measurer_.SetScheduler(nullptr);
    EXPECT_EQ(measurer_.attempts.load(), 2) << "無効化後は改めて試す";
}

// 測れない (nullopt) ノードも例外と同じく 1 回で諦め、描画のたびに測り直し続けない。
TEST_F(ThrowingMeasureTest, UnmeasurableNodeIsNotRetried)
{
    constexpr size_t kTarget = 8;
    measurer_.fail_with_nullopt = true;
    auto [nodes, cache] = LayoutParagraphs(30);

    cache[kTarget].layout_dirty = true;
    measurer_.throw_on = &nodes[kTarget];
    measurer_.SetScheduler(&task_scheduler_);
    DrainDirtyBatches(engine_, nodes, cache);
    measurer_.SetScheduler(nullptr);

    EXPECT_EQ(measurer_.attempts.load(), 1);
    EXPECT_FALSE(cache[kTarget].layout_dirty);
    EXPECT_FALSE(cache.HasDirtyNodes());
    EXPECT_TRUE(YChainConsistent(nodes, cache, theme_));
}

// 測れなかった (例外でなく nullopt) ノードは推定の高さを実測値として残さない。
// 残すと同じ幅に戻ったとき EstimateInvisibleNodeHeight が推定値を実測値とみなす。
TEST(MeasureEntryTest, UnmeasurableNodeCommitsNothing)
{
    struct FailingMeasurer : MockTextMeasurer {
        std::optional<float> MeasureNode(const Node&, NodeLayoutEntry&, float) const override
        {
            return std::nullopt;
        }
    } measurer;
    const auto nodes = ParseNodes("text");
    NodeLayoutEntry entry;
    entry.height = 42.0f;

    MeasureEntry(measurer, nodes[0], entry, 300.0f, nullptr);

    EXPECT_TRUE(entry.layout_dirty);
    EXPECT_FALSE(entry.is_measured());
    EXPECT_FLOAT_EQ(entry.height, 42.0f);
}

TEST_F(DirtyBatchSerialTest, NoClipProcessesAllDirtyEvenIfYUnreachable)
{
    // viewport_clip top<0 で全 dirty 対象。y_position の値に関わらず処理。
    DirtyNodeFixture f;
    f.Build(5, { 0, 4 });
    f.cache.SetTop(0, -1000.0f); // 大きく外れた値
    f.cache.SetTop(4, 999999.0f);
    const auto r = RunBatch(f, theme_, mock_, YRange::All(), kNoLayoutDeadline, nullptr);
    EXPECT_EQ(r.processed, 2);
    EXPECT_FALSE(f.cache[0].layout_dirty);
    EXPECT_FALSE(f.cache[4].layout_dirty);
    EXPECT_FALSE(r.more);
}

namespace {

// 全ノードを MeasureIndicesParallel で計測し、計測できなかったノード数を返す。scheduler が null なら直列に計測する。
int MeasureAll(std::vector<Node>& nodes, LayoutCache& cache, const Theme& theme, ITextMeasurer& measurer, TaskScheduler* scheduler)
{
    std::vector<size_t> indices(nodes.size());
    std::ranges::iota(indices, size_t{ 0 });
    measurer.SetTheme(theme);
    measurer.SetScheduler(scheduler);
    const int left = layout::MeasureIndicesParallel(nodes, cache, 800.0f, measurer, indices, kNoLayoutDeadline);
    measurer.SetScheduler(nullptr);
    return left;
}

// CodeBlock の syntax_tokens を書く派生 mock。並列経路では worker から entry へ直接書かれる。
class MockTextMeasurerWithTokens : public MockTextMeasurer {
public:
    std::optional<float> MeasureNode(const Node& node, NodeLayoutEntry& entry, float max_width) const override
    {
        const auto height = MockTextMeasurer::MeasureNode(node, entry, max_width);

        if (node.type != NodeType::CodeBlock) {
            return height;
        }
        const size_t seed = node.SourceOffset();
        const auto seed32 = static_cast<uint32_t>(seed);
        const auto MakeTok = [seed32](uint32_t k) {
            return SyntaxToken{
                .start = seed32 + k * 10u,
                .length = (seed32 % 7u) + 1u + k,
                .type = static_cast<SyntaxTokenType>((seed32 + k) % 4u),
            };
        };
        entry.set_syntax_tokens({ MakeTok(0), MakeTok(1), MakeTok(2) });
        return height;
    }
};

// Serial / Parallel の出力を比較。height (FLOAT_EQ) と layout_dirty / syntax_tokens を観測する。
// IDWriteTextLayout (entry.text_layout) は mock では nullptr 固定 / 実装では identity 不一致で
// equality が無意味なため除外。total_height_ は LayoutEngine 経由でないと
// 計算されないためスコープ外。
void ExpectBitExactEqual(const StressFixture& s, const StressFixture& p)
{
    ASSERT_EQ(s.nodes.size(), p.nodes.size());
    const size_t n = s.nodes.size();
    for (size_t i = 0; i < n; ++i) {
        EXPECT_FLOAT_EQ(s.cache[i].height, p.cache[i].height) << "i=" << i;
        EXPECT_EQ(s.cache[i].layout_dirty, p.cache[i].layout_dirty) << "i=" << i;

        const auto ts = s.cache[i].syntax_tokens();
        const auto tp = p.cache[i].syntax_tokens();
        ASSERT_EQ(ts.size(), tp.size()) << "syntax_tokens size mismatch i=" << i;
        for (size_t k = 0; k < ts.size(); ++k) {
            EXPECT_EQ(ts[k].start, tp[k].start) << "i=" << i << " k=" << k;
            EXPECT_EQ(ts[k].length, tp[k].length) << "i=" << i << " k=" << k;
            EXPECT_EQ(ts[k].type, tp[k].type) << "i=" << i << " k=" << k;
        }
    }
}

class ParallelMeasureStressTest : public ::testing::Test {
protected:
    MockTextMeasurerWithTokens mock_;
    Theme theme_ = GetLightTheme();
};

class ParallelWorkerCountTest : public ParallelMeasureStressTest, public ::testing::WithParamInterface<int> {};

} // namespace

// ===== 22000 ノード stress + bit-exact 検証 =====

TEST_F(ParallelMeasureStressTest, MatchesSerialOutputOnLargeMixedFixture)
{
    TaskScheduler ts{ 2 };
    constexpr size_t N = 22000;
    StressFixture s;
    s.Build(N, /*include_code_block=*/false);
    StressFixture p;
    p.Build(N, /*include_code_block=*/false);

    const int rs = MeasureAll(s.nodes, s.cache, theme_, mock_, nullptr);
    const int rp = MeasureAll(p.nodes, p.cache, theme_, mock_, &ts);

    EXPECT_EQ(rs, 0);
    EXPECT_EQ(rp, 0);
    ExpectBitExactEqual(s, p);
}

// ===== worker 数変動 (0 は全 chunk を呼び出し側で回す) =====

TEST_P(ParallelWorkerCountTest, BitExactAcrossWorkerCount)
{
    const int worker_count = GetParam();
    TaskScheduler ts{ worker_count };
    EXPECT_EQ(ts.WorkerCount(), static_cast<size_t>(worker_count));

    constexpr size_t N = 2000;
    StressFixture s;
    s.Build(N, /*include_code_block=*/true);
    StressFixture p;
    p.Build(N, /*include_code_block=*/true);

    const int rs = MeasureAll(s.nodes, s.cache, theme_, mock_, nullptr);
    const int rp = MeasureAll(p.nodes, p.cache, theme_, mock_, &ts);

    EXPECT_EQ(rs, 0);
    EXPECT_EQ(rp, 0);
    ExpectBitExactEqual(s, p);

    // 全 CodeBlock ノードが 3 トークンを持ち、token.start = source_offset + k*10 で並ぶこと。
    size_t code_block_count = 0;
    for (size_t i = 0; i < N; ++i) {
        EXPECT_FALSE(p.cache[i].layout_dirty) << "i=" << i;
        EXPECT_GT(p.cache[i].height, 0.0f) << "i=" << i;
        if (p.nodes[i].type != NodeType::CodeBlock) {
            continue;
        }
        ++code_block_count;
        const auto tokens = p.cache[i].syntax_tokens();
        ASSERT_EQ(tokens.size(), 3u) << "i=" << i;
        const uint32_t seed = static_cast<uint32_t>(i);
        for (uint32_t k = 0; k < 3; ++k) {
            EXPECT_EQ(tokens[k].start, seed + k * 10u) << "i=" << i << " k=" << k;
        }
    }
    // 含有率 10% (bucket 0,1 = N/10) を確認。fixture 構成変更時の sentinel。
    EXPECT_EQ(code_block_count, N / 10);
}

INSTANTIATE_TEST_SUITE_P(WorkerCounts, ParallelWorkerCountTest, ::testing::Values(0, 1, 2, 8, 16));

// ============================================================
// ProcessDirtyBatch ビューポート制限テスト
// ============================================================

class ProcessDirtyBatchViewportTest : public MockLayoutTestBase {
protected:
    // 一度組んで Y 位置を確定させてから、全ノードを evict した状態にする。
    ParsedLayout LayoutThenEvictAll(int paragraphs)
    {
        auto pl = ParseAndLayout(MakeParagraphs(paragraphs));
        for (size_t i = 0; i < pl.nodes.size(); i++) {
            pl.cache[i].layout_dirty = true;
            pl.cache[i].release_layout_state();
        }
        return pl;
    }
};

TEST_F(ProcessDirtyBatchViewportTest, SkipsFarOffscreenNodes)
{
    auto [nodes, cache] = LayoutThenEvictAll(100);
    ASSERT_GT(nodes.size(), 50u);

    // ビューポートを先頭に設定（top=0, height=200）
    // バッファ: ±5画面 = ±1000px → [−1000, 1200] が処理範囲
    const float viewport_top = 0.0f;
    const float viewport_height = 200.0f;

    // ビューポート制限付きで ProcessDirtyBatch を実行
    engine_.ProcessDirtyBatch(nodes, cache, 800.0f, kNoLayoutDeadline, layout::BufferedRange(viewport_top, viewport_height, 5.0f));

    // ビューポート付近のノードはダーティでなくなっているはず
    EXPECT_FALSE(cache[0].layout_dirty) << "ビューポート内のノードは処理されるべき";

    // ビューポートから遠いノード（y > 1200）はダーティのまま
    const bool found_far_dirty = std::ranges::any_of(std::views::iota(size_t{ 0 }, nodes.size()), [&](size_t i) {
        return cache.Top(i) > 1200.0f && cache[i].layout_dirty;
    });
    EXPECT_TRUE(found_far_dirty) << "ビューポート遠方のノードは未処理のままであるべき";
}

TEST_F(ProcessDirtyBatchViewportTest, WithoutViewportLimitProcessesAll)
{
    auto [nodes, cache] = LayoutThenEvictAll(50);

    // ビューポート制限なし (clip 省略)
    engine_.ProcessDirtyBatch(nodes, cache, 800.0f, kNoLayoutDeadline, YRange::All());

    // 全ノードが処理されるべき
    for (size_t i = 0; i < nodes.size(); i++) {
        EXPECT_FALSE(cache[i].layout_dirty) << "ノード " << i << " は処理されるべき";
    }
}

TEST_F(ProcessDirtyBatchViewportTest, HasDirtyNodesFalseAfterNearbyProcessed)
{
    auto [nodes, cache] = LayoutThenEvictAll(100);

    // ビューポート制限付きで全バッチ処理
    bool more = true;
    int iterations = 0;
    while (more && iterations < 100) {
        more = engine_.ProcessDirtyBatch(nodes, cache, 800.0f, kNoLayoutDeadline, layout::BufferedRange(0.0f, 200.0f, 5.0f));
        iterations++;
    }

    // 付近のダーティノードが処理され、has_dirty_nodes が false になるべき
    EXPECT_FALSE(cache.HasDirtyNodes())
        << "ビューポート付近の全ダーティノードが処理されたら false であるべき";
}
