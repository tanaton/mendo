// 部分レイアウト・ダーティ処理・エビクトを乱数の操作列で混ぜ、不変条件と
// 全再計算との一致を確かめるモデルベーステスト。
#include <gtest/gtest.h>
#include <algorithm>
#include <cmath>
#include <deque>
#include <memory_resource>
#include <random>
#include <string>
#include "document.h"
#include "document_test_helpers.h"
#include "layout.h"
#include "layout_invariants.h"
#include "mock_text_measurer.h"
#include "parser.h"
#include "task_scheduler.h"
#include "test_helpers.h"
#include "test_nodes.h"
#include "viewport_manager.h"

using layout::EstimateNodeHeights;
using layout::NodeIndent;

namespace {

constexpr float kYEps = 0.05f;
// Reestimate が再計測を省く幅変化の上限。
constexpr float kWidthHysteresis = LayoutEngine::kWidthChangeThreshold;

// 計測したエントリに目印 (行追跡の無い table_layout) を残す。EvictEntryLayout はこれを
// 破棄するので、keep 範囲外に目印が残っていれば解放漏れと分かる。
class MarkingMeasurer : public MockTextMeasurer {
public:
    void MeasureNode(const Node& node, NodeLayoutEntry& entry, float max_width,
                     MeasureViewportRange viewport = {}) const override
    {
        MockTextMeasurer::MeasureNode(node, entry, max_width, viewport);
        entry.ensure_table_layout();
    }
};

std::string Words(std::mt19937& rng, int max_words)
{
    std::string s;
    const int n = static_cast<int>(rng() % static_cast<uint32_t>(max_words + 1));
    for (int i = 0; i < n; ++i) {
        s += std::string(1 + rng() % 9, static_cast<char>('a' + rng() % 26));
        s += ' ';
    }
    return s;
}

// 見出し・コード・ダイアグラム・空 LI (loose list)・タスク・表・HR・引用・画像・段落の混在文書。
std::string MakeRandomMarkdown(std::mt19937& rng, int blocks)
{
    std::string md;
    for (int b = 0; b < blocks; ++b) {
        switch (rng() % 10) {
        case 0:
            md += std::string(1 + rng() % 6, '#') + " Heading " + Words(rng, 6) + "\n\n";
            break;
        case 1: {
            md += "```cpp\n";
            const int lines = 1 + static_cast<int>(rng() % 6);
            for (int l = 0; l < lines; ++l) {
                md += "int v" + std::to_string(l) + " = " + std::to_string(rng() % 1000) + ";\n";
            }
            md += "```\n\n";
            break;
        }
        case 2:
            md += "```mermaid\ngraph TD; A-->B\n```\n\n";
            break;
        case 3:
            md += "- " + Words(rng, 10) + "x\n\n- " + Words(rng, 30) + "y\n\n";
            break;
        case 4:
            md += "- [ ] " + Words(rng, 8) + "t\n- [x] done\n\n";
            break;
        case 5: {
            md += "| h1 | h2 | h3 |\n|---|---|---|\n";
            const int rows = 1 + static_cast<int>(rng() % 6);
            for (int r = 0; r < rows; ++r) {
                md += "| " + Words(rng, 3) + "a | b | c |\n";
            }
            md += "\n";
            break;
        }
        case 6:
            md += "---\n\n";
            break;
        case 7:
            md += "> " + Words(rng, 20) + "q\n\n";
            break;
        case 8:
            md += "![img](x.png)\n\n";
            break;
        default:
            md += Words(rng, 120) + "p\n\n";
            break;
        }
    }
    return md;
}

class IncrementalLayoutTest : public ::testing::Test {
protected:
    static constexpr float kViewportHeight = 600.0f;

    MarkingMeasurer measurer_;
    Theme theme_{};
    LayoutEngine engine_;
    TaskScheduler scheduler_;
    std::pmr::vector<Node> nodes_;
    LayoutCache cache_;
    float width_ = 800.0f;
    float scroll_ = 0.0f;

    void SetUp() override
    {
        theme_ = GetLightTheme();
        ASSERT_TRUE(engine_.Init(&measurer_, theme_));
        scheduler_.Init(2);
    }
    void TearDown() override
    {
        engine_.SetLayoutScheduler(nullptr);
        scheduler_.Shutdown();
    }

    // アプリのファイル読込と同じく推定高さで並べてから可視範囲だけ計測する。
    void Load(std::mt19937& rng, int blocks)
    {
        nodes_ = ParseNodes(sources_.emplace_back(MakeRandomMarkdown(rng, blocks)));
        cache_ = layout::MakeEstimatedLayoutCache(nodes_, theme_);
        width_ = 800.0f;
        scroll_ = 0.0f;
        ComputeLayout(engine_, nodes_, cache_, width_, scroll_, scroll_ + kViewportHeight);
    }

    float MaxScroll() const
    {
        return std::max(0.0f, ComputeTotalContentHeight(cache_, theme_.margin_bottom) - kViewportHeight);
    }

    float NodeWidth(size_t i) const
    {
        return theme_.ContentWidth(width_) - NodeIndent(nodes_[i], theme_);
    }

    // 計測幅も現在幅も「最後に再計測を決めた幅」から 2px 以内なので、計測済みノードのずれは高々その 2 倍。
    // ダーティ処理のたびに基準幅を進めると、小刻みなリサイズでずれが際限なく積み重なる。
    ::testing::AssertionResult CleanNodesMeasuredNearCurrentWidth() const
    {
        for (size_t i = 0; i < nodes_.size(); ++i) {
            if (cache_[i].layout_dirty) {
                continue;
            }
            const float drift = std::abs(cache_[i].cached_width - layout::NodeLayoutWidth(nodes_[i], NodeWidth(i)));
            if (drift > 2.0f * kWidthHysteresis + 0.01f) {
                return ::testing::AssertionFailure() << "i=" << i << " は古い幅のまま残っている (ずれ " << drift << "px)";
            }
        }
        return ::testing::AssertionSuccess();
    }

    enum class Op {
        Resize,
        Drag,
        Scroll,
        Layout,
        DirtyBatch,
        MarkDirty,
        Evict,
        Count,
    };

    // 1 操作を適用し、再現用の説明を返す。Evict の keep 範囲は keep_out に返す。
    std::string Apply(std::mt19937& rng, Op op, VisibleRange& keep_out)
    {
        engine_.SetLayoutScheduler(rng() % 2 == 0 ? &scheduler_ : nullptr);
        switch (op) {
        case Op::Resize: {
            const float magnitude = static_cast<float>(1 + rng() % 50);
            Resize(rng() % 2 == 0 ? magnitude : -magnitude);
            return "Resize width=" + std::to_string(width_);
        }
        case Op::Drag: {
            // ウィンドウ端のドラッグ: 2px 未満 (再計測を省くヒステリシス内) の WM_SIZE が続き、
            // 合間にタイマーのダーティ処理やスクロールが挟まる。
            const float dir = rng() % 2 == 0 ? 1.0f : -1.0f;
            const int steps = 2 + static_cast<int>(rng() % 7);
            std::string desc = "Drag";
            for (int k = 0; k < steps; ++k) {
                Resize(dir * std::uniform_real_distribution<float>(0.3f, 1.9f)(rng));
                desc += " w=" + std::to_string(width_);
                switch (rng() % 3) {
                case 0:
                    // アプリのタイマーと同じバッチ上限。
                    engine_.ProcessDirtyBatch(nodes_, cache_, width_, 200, { scroll_, kViewportHeight, 1.0f });
                    desc += "+batch";
                    break;
                case 1:
                    ScrollTo(scroll_ + std::uniform_real_distribution<float>(-300.0f, 300.0f)(rng));
                    desc += "+scroll=" + std::to_string(scroll_);
                    break;
                default:
                    break;
                }
            }
            return desc;
        }
        case Op::Scroll:
            ScrollTo(MaxScroll() > 0.0f ? std::uniform_real_distribution<float>(0.0f, MaxScroll())(rng) : 0.0f);
            return "Scroll+EnsureVisibleLayout scroll=" + std::to_string(scroll_);
        case Op::Layout:
            ComputeLayout(engine_, nodes_, cache_, width_, scroll_, scroll_ + kViewportHeight);
            return "ComputeLayout";
        case Op::DirtyBatch: {
            const int batch = 1 + static_cast<int>(rng() % 5);
            if (rng() % 2 == 0) {
                const float buffer = static_cast<float>(rng() % 3);
                engine_.ProcessDirtyBatch(nodes_, cache_, width_, batch, { scroll_, kViewportHeight, buffer });
                return "ProcessDirtyBatch batch=" + std::to_string(batch) + " clip buffer=" + std::to_string(buffer);
            }
            engine_.ProcessDirtyBatch(nodes_, cache_, width_, batch);
            return "ProcessDirtyBatch batch=" + std::to_string(batch) + " noclip";
        }
        case Op::MarkDirty: {
            const size_t i = rng() % nodes_.size();
            cache_[i].layout_dirty = true;
            return "MarkDirty i=" + std::to_string(i);
        }
        case Op::Evict: {
            const float buffer = kViewportHeight * static_cast<float>(rng() % 3);
            keep_out = ComputeVisibleNodeRange(cache_, scroll_ - buffer, scroll_ + kViewportHeight + buffer);
            cache_.EvictTextLayouts(keep_out.first, keep_out.last_plus_1);
            return "Evict keep=[" + std::to_string(keep_out.first) + "," + std::to_string(keep_out.last_plus_1) + ")";
        }
        case Op::Count:
            break;
        }
        return {};
    }

    void Resize(float delta)
    {
        width_ = std::clamp(width_ + delta, 300.0f, 1600.0f);
        ComputeLayout(engine_, nodes_, cache_, width_, scroll_, scroll_ + kViewportHeight);
    }

    void ScrollTo(float y)
    {
        scroll_ = std::clamp(y, 0.0f, MaxScroll());
        engine_.EnsureVisibleLayout(nodes_, cache_, width_, scroll_, scroll_ + kViewportHeight);
    }

    // ProcessDirtyBatch が残り dirty なしを返すまで直列で処理し切る。
    void Settle()
    {
        engine_.SetLayoutScheduler(nullptr);
        for (int guard = 0; guard < 10000; ++guard) {
            if (!engine_.ProcessDirtyBatch(nodes_, cache_, width_, 0)) {
                return;
            }
        }
        ADD_FAILURE() << "ProcessDirtyBatch が収束しない";
    }

    Op RandomOp(std::mt19937& rng) const
    {
        return static_cast<Op>(rng() % static_cast<uint32_t>(Op::Count));
    }

private:
    std::deque<std::string> sources_;
};

} // namespace

// 前回 evict 以降に keep 範囲外で計測されたノード (スクロール先の EnsureVisibleLayout、
// クリップ無し ProcessDirtyBatch、幅変更 ComputeLayout) も次の evict で解放されること。
TEST_F(IncrementalLayoutTest, EvictReleasesEverythingOutsideKeep)
{
    for (uint32_t seed = 101; seed <= 110; ++seed) {
        std::mt19937 rng(seed);
        Load(rng, 120);
        ASSERT_GT(nodes_.size(), 100u);
        for (int step = 0; step < 400; ++step) {
            VisibleRange keep{};
            const Op op = RandomOp(rng);
            const std::string desc = Apply(rng, op, keep);
            if (op != Op::Evict) {
                continue;
            }
            SCOPED_TRACE("seed=" + std::to_string(seed) + " step=" + std::to_string(step) + " " + desc);
            for (size_t i = 0; i < nodes_.size(); ++i) {
                if (i >= keep.first && i < keep.last_plus_1) {
                    continue;
                }
                ASSERT_EQ(cache_[i].table_layout(), nullptr) << "keep 範囲外 i=" << i << " の layout が残っている";
            }
        }
    }
}

// どの操作の後も Y 連鎖 (Top(i+1) = Bottom(i) + sb(i) + sa(i+1)) が保たれ、ダーティを処理し切れば
// 各ノードはヒステリシス幅以内の現在幅で計測済みになり、幅変更後は新規の全レイアウトと一致する。
TEST_F(IncrementalLayoutTest, MatchesFullLayoutAfterRandomOperations)
{
    for (uint32_t seed = 1; seed <= 12; ++seed) {
        std::mt19937 rng(seed);
        Load(rng, 100);
        ASSERT_GT(nodes_.size(), 80u);
        for (int step = 0; step < 300; ++step) {
            VisibleRange keep{};
            const std::string desc = Apply(rng, RandomOp(rng), keep);
            SCOPED_TRACE("seed=" + std::to_string(seed) + " step=" + std::to_string(step) + " " + desc);
            ASSERT_TRUE(YChainConsistent(nodes_, cache_, theme_));
            ASSERT_TRUE(CleanNodesMeasuredNearCurrentWidth());
        }

        SCOPED_TRACE("seed=" + std::to_string(seed) + " settle");
        Settle();
        ASSERT_TRUE(YChainConsistent(nodes_, cache_, theme_));
        ASSERT_TRUE(CleanNodesMeasuredNearCurrentWidth());
        for (size_t i = 0; i < nodes_.size(); ++i) {
            ASSERT_FALSE(cache_[i].layout_dirty) << "i=" << i;
            NodeLayoutEntry remeasured{};
            remeasured.height = cache_[i].height;
            MeasureEntry(measurer_, nodes_[i], remeasured, cache_[i].cached_width, {}, 0.0f, cache_.FindDiagram(i), theme_);
            ASSERT_FLOAT_EQ(cache_[i].height, remeasured.height) << "i=" << i;
        }

        // ヒステリシスを超える幅変更の後は、新規に全レイアウトした結果と一致する。
        width_ += 10.0f;
        ComputeLayout(engine_, nodes_, cache_, width_, scroll_, scroll_ + kViewportHeight);
        Settle();

        LayoutEngine ref_engine;
        ASSERT_TRUE(ref_engine.Init(&measurer_, theme_));
        LayoutCache ref;
        ref.Resize(nodes_.size());
        EstimateNodeHeights(nodes_, ref, theme_);
        ComputeLayout(ref_engine, nodes_, ref, width_);
        for (size_t i = 0; i < nodes_.size(); ++i) {
            ASSERT_NEAR(cache_[i].height, ref[i].height, kYEps) << "i=" << i;
            ASSERT_NEAR(cache_.Top(i), ref.Top(i), kYEps) << "i=" << i;
        }
    }
}

namespace {

// 計測したノードの番号を記録する。
class RecordingMeasurer : public MockTextMeasurer {
public:
    const Node* base = nullptr;
    mutable std::vector<size_t> measured;

    void MeasureNode(const Node& node, NodeLayoutEntry& entry, float max_width,
                     MeasureViewportRange viewport = {}) const override
    {
        measured.push_back(static_cast<size_t>(&node - base));
        MockTextMeasurer::MeasureNode(node, entry, max_width, viewport);
    }
};

} // namespace

// ズームで推定し直すと Y がずれる。古い scroll_y の位置ではなく、見ていたノードの周りを計測する。
TEST(LayoutServiceTest, ViewportLayoutMeasuresAroundScrollTargetAfterReestimate)
{
    constexpr float kWidth = 800.0f;
    constexpr float kHeight = 600.0f;
    constexpr int kTarget = 300;
    const Theme theme = GetLightTheme();
    RecordingMeasurer measurer;
    // 実測を推定より大きくして、推定し直すと Y が大きく縮むようにする。
    measurer.line_height = 80.0f;
    LayoutEngine engine;
    ASSERT_TRUE(engine.Init(&measurer, theme));
    ViewportManager viewport;
    LayoutService service(engine, viewport);

    auto doc = Document::FromMarkdown(std::pmr::string{ MakeParagraphs(400) }, L"t.md");
    measurer.base = doc.GetNodes().data();
    LayoutCache cache;
    ComputeLayout(engine, doc.GetNodes(), cache, kWidth);
    viewport.SetScrollTarget(kTarget, 0.0f);
    viewport.ApplyScrollTarget(cache);
    const float measured_scroll_y = viewport.GetScrollY();

    ASSERT_TRUE(engine.RecreateFormats());
    cache.InvalidateAllLayouts(doc.GetNodes());
    measurer.measured.clear();
    service.ViewportLayout(doc, cache, kWidth, kHeight);

    EXPECT_LT(viewport.GetScrollY(), measured_scroll_y) << "推定し直した Y で位置を求め直す";
    EXPECT_FLOAT_EQ(viewport.GetScrollY(), cache.Top(kTarget));
    const auto vr = ComputeVisibleNodeRange(cache, viewport.GetScrollY(), viewport.GetScrollY() + kHeight);
    for (size_t i = vr.first; i < vr.last_plus_1; ++i) {
        EXPECT_FALSE(cache[i].layout_dirty) << "可視ノードが未計測 i=" << i;
    }
    // 1 パス目は推定の Y で 1 画面分 (この文書では 20 ノード弱) を計測するので、実測の可視ノードより多くなる。
    constexpr size_t kEstimatedScreenNodes = 30;
    for (const size_t i : measurer.measured) {
        EXPECT_GE(i, vr.first) << "見ていた位置より上を計測した";
        EXPECT_LT(i, kTarget + kEstimatedScreenNodes) << "見ていた位置から離れた所を計測した";
    }
}

// worker の推定は幅を知らず段落を 1 行と見る。文書の最初のレイアウトで、未計測の段落を表示幅で推定し直す。
TEST(LayoutServiceTest, ViewportLayoutReestimatesWorkerEstimateWithWidth)
{
    constexpr float kWidth = 800.0f;
    const Theme theme = GetLightTheme();
    MockTextMeasurer measurer;
    LayoutEngine engine;
    ASSERT_TRUE(engine.Init(&measurer, theme));
    ViewportManager viewport;
    LayoutService service(engine, viewport);

    std::string md;
    for (int i = 0; i < 200; ++i) {
        md += std::string(2000, 'a') + "\n\n";
    }
    auto doc = Document::FromMarkdown(std::pmr::string{ md }, L"t.md");
    LayoutCache cache = layout::MakeEstimatedLayoutCache(doc.GetNodes(), theme);
    const size_t last = doc.GetNodes().size() - 1;
    const float width_free = cache[last].height;

    service.ViewportLayout(doc, cache, kWidth, 600.0f);

    ASSERT_TRUE(cache[last].layout_dirty) << "前提: 末尾は画面外で計測されていない";
    EXPECT_GT(cache[last].height, width_free);
    EXPECT_FLOAT_EQ(cache[last].height, layout::EstimateNodeHeight(doc.GetNodes()[last], theme, layout::NodeContentWidth(doc.GetNodes()[last], theme, theme.ContentWidth(kWidth))));
}

// 表示幅を渡した worker の推定は、UI スレッドで推定し直したものと同じ高さと位置になる。
TEST(LayoutServiceTest, WidthAwareWorkerEstimateMatchesReestimate)
{
    constexpr float kWidth = 800.0f;
    const Theme theme = GetLightTheme();
    MockTextMeasurer measurer;
    LayoutEngine engine;
    ASSERT_TRUE(engine.Init(&measurer, theme));

    std::string md;
    for (int i = 0; i < 50; ++i) {
        md += "# H\n\n" + std::string(static_cast<size_t>(100 + i * 40), 'a') + "\n\n```\ncode\n```\n\n";
    }
    auto doc = Document::FromMarkdown(std::pmr::string{ md }, L"t.md");
    LayoutCache reestimated = layout::MakeEstimatedLayoutCache(doc.GetNodes(), theme);
    engine.Reestimate(doc.GetNodes(), reestimated, kWidth);
    LayoutCache width_aware = layout::MakeEstimatedLayoutCache(doc.GetNodes(), theme, theme.ContentWidth(kWidth));
    const auto basis = width_aware.GetEstimateBasis();
    EXPECT_FLOAT_EQ(basis.content_width, theme.ContentWidth(kWidth));

    ASSERT_EQ(width_aware.size(), reestimated.size());
    for (size_t i = 0; i < reestimated.size(); ++i) {
        EXPECT_FLOAT_EQ(width_aware[i].height, reestimated[i].height) << i;
        EXPECT_FLOAT_EQ(width_aware.Top(i), reestimated.Top(i)) << i;
    }
}

// リロードで中身が変わらないノードの計測結果を引き継ぎ、変わったノードだけ計測し直させる。
TEST(CarryOverUnchangedTest, KeepsMeasuredEntriesOfUnchangedNodes)
{
    const Theme theme = GetLightTheme();
    MockTextMeasurer measurer;
    LayoutEngine engine;
    ASSERT_TRUE(engine.Init(&measurer, theme));
    LayoutSourceStore store;
    auto [old_nodes, old_cache] = store.ParseAndLayout(engine, "# Title\n\nfirst\n\nsecond\n\nthird\n", 800.0f);
    auto [new_nodes, new_cache] = store.ParseAndLayout(engine, "# Title\n\nfirst\n\nsecond edited\n\nthird\n", 800.0f);
    ASSERT_EQ(old_nodes.size(), 4u);
    ASSERT_EQ(new_nodes.size(), 4u);
    const float title_height = old_cache[0].height;
    new_cache = layout::MakeEstimatedLayoutCache(new_nodes, theme);

    EXPECT_EQ(layout::CarryOverUnchanged(old_nodes, old_cache, new_nodes, new_cache).carried, 3u);
    EXPECT_FALSE(new_cache[0].layout_dirty);
    EXPECT_FLOAT_EQ(new_cache[0].height, title_height);
    EXPECT_FALSE(new_cache[1].layout_dirty);
    EXPECT_TRUE(new_cache[2].layout_dirty) << "変わったノードは計測し直す";
    EXPECT_FALSE(new_cache[3].layout_dirty);
}

// 末尾の参照定義が変わると、前のノードはテキストが同じでもリンク先が変わるので引き継がない。
TEST(CarryOverUnchangedTest, LinkTargetChangeIsNotCarried)
{
    const Theme theme = GetLightTheme();
    MockTextMeasurer measurer;
    LayoutEngine engine;
    ASSERT_TRUE(engine.Init(&measurer, theme));
    LayoutSourceStore store;
    auto [old_nodes, old_cache] = store.ParseAndLayout(engine, "see [doc][r]\n\ntail\n\n[r]: https://a.example/\n", 800.0f);
    auto [new_nodes, new_cache] = store.ParseAndLayout(engine, "see [doc][r]\n\ntail\n\n[r]: https://b.example/\n", 800.0f);
    ASSERT_EQ(old_nodes.size(), new_nodes.size());
    new_cache = layout::MakeEstimatedLayoutCache(new_nodes, theme);

    layout::CarryOverUnchanged(old_nodes, old_cache, new_nodes, new_cache);
    EXPECT_TRUE(new_cache[0].layout_dirty);
    EXPECT_FALSE(new_cache[1].layout_dirty);
}
