// 部分レイアウト・ダーティ処理・エビクトを乱数の操作列で混ぜ、不変条件と
// 全再計算との一致を確かめるモデルベーステスト。
#include <gtest/gtest.h>
#include <algorithm>
#include <cmath>
#include <deque>
#include <random>
#include <string>
#include "d2d_util.h"
#include "document.h"
#include "document_test_helpers.h"
#include "layout.h"
#include "mermaid_util.h"
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

// 計測したエントリに目印の text_layout を残す。EvictEntryLayout はこれを
// 破棄するので、keep 範囲外に目印が残っていれば解放漏れと分かる。
class MarkingMeasurer : public MockTextMeasurer {
public:
    std::optional<float> MeasureNode(const Node& node, NodeLayoutEntry& entry, float max_width) const override
    {
        const auto height = MockTextMeasurer::MeasureNode(node, entry, max_width);
        if (node.type != NodeType::Table) {
            entry.set_text_layout(Marker());
        }
        return height;
    }

private:
    static Microsoft::WRL::ComPtr<IDWriteTextLayout> Marker()
    {
        static const auto layout = [] {
            IDWriteFactory* const factory = SharedDWriteFactory();
            const auto format = CreateTextFormat(factory, { .family = L"Segoe UI", .size = 12.0f, .locale = L"en-us" });
            Microsoft::WRL::ComPtr<IDWriteTextLayout> text_layout;
            factory->CreateTextLayout(L"m", 1, format.Get(), 100.0f, 100.0f, &text_layout);
            return text_layout;
        }();
        return layout;
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
    TaskScheduler scheduler_{ 2 };
    std::vector<Node> nodes_;
    LayoutCache cache_;
    float width_ = 800.0f;
    float scroll_ = 0.0f;

    void SetUp() override
    {
        theme_ = GetLightTheme();
        ASSERT_TRUE(engine_.Init(&measurer_, theme_));
    }
    void TearDown() override
    {
        measurer_.SetScheduler(nullptr);
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
    std::string Apply(std::mt19937& rng, Op op, IndexRange& keep_out)
    {
        measurer_.SetScheduler(rng() % 2 == 0 ? &scheduler_ : nullptr);
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
                    engine_.ProcessDirtyBatch(nodes_, cache_, width_, kNoLayoutDeadline, layout::BufferedRange(scroll_, kViewportHeight, 1.0f));
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
            // 期限切れ (1 ノードだけ) か期限なし。
            const bool expired = rng() % 2 == 0;
            const LayoutDeadline deadline = expired ? LayoutDeadline{} : kNoLayoutDeadline;
            const std::string desc = std::string("ProcessDirtyBatch ") + (expired ? "expired" : "unlimited");
            if (rng() % 2 == 0) {
                const float buffer = static_cast<float>(rng() % 3);
                engine_.ProcessDirtyBatch(nodes_, cache_, width_, deadline, layout::BufferedRange(scroll_, kViewportHeight, buffer));
                return desc + " clip buffer=" + std::to_string(buffer);
            }
            engine_.ProcessDirtyBatch(nodes_, cache_, width_, deadline, YRange::All());
            return desc + " noclip";
        }
        case Op::MarkDirty: {
            const size_t i = rng() % nodes_.size();
            cache_[i].layout_dirty = true;
            return "MarkDirty i=" + std::to_string(i);
        }
        case Op::Evict: {
            const float buffer = kViewportHeight * static_cast<float>(rng() % 3);
            keep_out = ComputeVisibleNodeRange(cache_, scroll_ - buffer, scroll_ + kViewportHeight + buffer);
            cache_.EvictTextLayouts(keep_out.begin, keep_out.end);
            return "Evict keep=[" + std::to_string(keep_out.begin) + "," + std::to_string(keep_out.end) + ")";
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
        measurer_.SetScheduler(nullptr);
        for (int guard = 0; guard < 10000; ++guard) {
            if (!engine_.ProcessDirtyBatch(nodes_, cache_, width_, kNoLayoutDeadline, YRange::All()).remaining) {
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
            IndexRange keep{};
            const Op op = RandomOp(rng);
            const std::string desc = Apply(rng, op, keep);
            if (op != Op::Evict) {
                continue;
            }
            SCOPED_TRACE("seed=" + std::to_string(seed) + " step=" + std::to_string(step) + " " + desc);
            for (size_t i = 0; i < nodes_.size(); ++i) {
                if (i >= keep.begin && i < keep.end) {
                    continue;
                }
                // 行の幾何を持つ表は、作り直しを避けるため幾何だけ残してセルの layout を捨てる。
                if (const auto* tl = cache_[i].table_layout()) {
                    ASSERT_TRUE(std::ranges::none_of(std::views::iota(size_t{ 0 }, tl->CellCount()), [&](size_t ci) { return tl->CellLayout(ci) != nullptr; }))
                        << "keep 範囲外 i=" << i << " のセルの layout が残っている";
                    continue;
                }
                ASSERT_EQ(cache_[i].text_layout(), nullptr) << "keep 範囲外 i=" << i << " の layout が残っている";
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
            IndexRange keep{};
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
            MeasureEntry(measurer_, nodes_[i], remeasured, cache_[i].cached_width, cache_.FindDiagram(i));
            ASSERT_FLOAT_EQ(cache_[i].height, remeasured.height) << "i=" << i;
        }

        // ヒステリシスを超える幅変更の後は、新規に全レイアウトした結果と一致する。
        width_ += 10.0f;
        ComputeLayout(engine_, nodes_, cache_, width_, scroll_, scroll_ + kViewportHeight);
        Settle();

        LayoutEngine ref_engine;
        ASSERT_TRUE(ref_engine.Init(&measurer_, theme_));
        LayoutCache ref(nodes_.size());
        EstimateNodeHeights(nodes_, ref, theme_, 0.0f, {});
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

    std::optional<float> MeasureNode(const Node& node, NodeLayoutEntry& entry, float max_width) const override
    {
        measured.push_back(static_cast<size_t>(&node - base));
        return MockTextMeasurer::MeasureNode(node, entry, max_width);
    }
};

} // namespace

// LayoutService と、リロードで計測結果を引き継ぐ処理。
class LayoutServiceTest : public MockLayoutTestBase {
protected:
    RecordingMeasurer recording_;
    ViewportManager viewport_;
    LayoutService service_{ engine_, viewport_ };

    // 計測したノードを記録する計測器で組む。
    void UseRecordingMeasurer()
    {
        ASSERT_TRUE(engine_.Init(&recording_, theme_));
    }
};

// ズームで推定し直すと Y がずれる。古い scroll_y の位置ではなく、見ていたノードの周りを計測する。
TEST_F(LayoutServiceTest, ViewportLayoutMeasuresAroundScrollTargetAfterReestimate)
{
    constexpr float kWidth = 800.0f;
    constexpr float kHeight = 600.0f;
    constexpr int kTarget = 300;
    // 実測を推定より大きくして、推定し直すと Y が大きく縮むようにする。
    recording_.line_height = 80.0f;
    UseRecordingMeasurer();

    auto doc = Document::FromMarkdown(MakeParagraphs(400), L"t.md");
    recording_.base = doc.GetNodes().data();
    LayoutCache cache(doc.GetNodes().size());
    ComputeLayout(engine_, doc.GetNodes(), cache, kWidth);
    viewport_.SetScrollTarget(kTarget, 0.0f);
    viewport_.ApplyScrollTarget(cache);
    const float measured_scroll_y = viewport_.GetScrollY();

    ASSERT_TRUE(recording_.RecreateFormats());
    cache.InvalidateAllLayouts(doc.GetNodes());
    recording_.measured.clear();
    service_.ViewportLayout(doc, cache, kWidth, kHeight);

    EXPECT_LT(viewport_.GetScrollY(), measured_scroll_y) << "推定し直した Y で位置を求め直す";
    EXPECT_FLOAT_EQ(viewport_.GetScrollY(), cache.Top(kTarget));
    const auto vr = ComputeVisibleNodeRange(cache, viewport_.GetScrollY(), viewport_.GetScrollY() + kHeight);
    for (size_t i = vr.begin; i < vr.end; ++i) {
        EXPECT_FALSE(cache[i].layout_dirty) << "可視ノードが未計測 i=" << i;
    }
    // 1 パス目は推定の Y で 1 画面分 (この文書では 20 ノード弱) を計測するので、実測の可視ノードより多くなる。
    constexpr size_t kEstimatedScreenNodes = 30;
    for (const size_t i : recording_.measured) {
        EXPECT_GE(i, vr.begin) << "見ていた位置より上を計測した";
        EXPECT_LT(i, kTarget + kEstimatedScreenNodes) << "見ていた位置から離れた所を計測した";
    }
}

// 本文ペインの高さが 0 (ウィンドウをタイトルバーまで縮めた) でも、遅延レイアウトは文書全体を測らない。
TEST_F(LayoutServiceTest, ProcessDirtyBatchWithZeroHeightStaysAtScrollPosition)
{
    constexpr float kWidth = 800.0f;
    UseRecordingMeasurer();

    auto doc = Document::FromMarkdown(MakeParagraphs(400), L"t.md");
    recording_.base = doc.GetNodes().data();
    LayoutCache cache = layout::MakeEstimatedLayoutCache(doc.GetNodes(), theme_);

    service_.ProcessDirtyBatch(doc, cache, kWidth, kNoLayoutDeadline, 0.0f, 2.0f);

    EXPECT_LE(recording_.measured.size(), 2u);
    EXPECT_TRUE(cache[doc.GetNodes().size() - 1].layout_dirty);
}

// 図の描き直し (ダークモードの切り替えなど) で画面より上の高さが変わっても、target が無くても見ている内容は動かない。
TEST_F(LayoutServiceTest, RecomputeAfterDiagramKeepsViewWithoutTarget)
{
    UseRecordingMeasurer();

    auto doc = Document::FromMarkdown(MakeParagraphs(100), L"t.md");
    LayoutCache cache = layout::MakeEstimatedLayoutCache(doc.GetNodes(), theme_);
    constexpr size_t kVisible = 50;
    viewport_.SetScrollY(cache.Top(kVisible) + 3.0f);
    ASSERT_FALSE(viewport_.GetScrollTarget().IsValid());

    constexpr float kGrowth = 500.0f;
    cache[1].height += kGrowth;
    IndexRange changed;
    changed.Add(1);
    service_.RecomputeAfterDiagram(doc, cache, changed);

    EXPECT_FLOAT_EQ(viewport_.GetScrollY(), cache.Top(kVisible) + 3.0f);
}

// worker の推定は幅を知らず段落を 1 行と見る。文書の最初のレイアウトで、未計測の段落を表示幅で推定し直す。
TEST_F(LayoutServiceTest, ViewportLayoutReestimatesWorkerEstimateWithWidth)
{
    constexpr float kWidth = 800.0f;

    std::string md;
    for (int i = 0; i < 200; ++i) {
        md += std::string(2000, 'a') + "\n\n";
    }
    auto doc = Document::FromMarkdown(std::string{ md }, L"t.md");
    LayoutCache cache = layout::MakeEstimatedLayoutCache(doc.GetNodes(), theme_);
    const size_t last = doc.GetNodes().size() - 1;
    const float width_free = cache[last].height;

    service_.ViewportLayout(doc, cache, kWidth, 600.0f);

    ASSERT_TRUE(cache[last].layout_dirty) << "前提: 末尾は画面外で計測されていない";
    EXPECT_GT(cache[last].height, width_free);
    EXPECT_FLOAT_EQ(cache[last].height, layout::EstimateNodeHeight(doc.GetNodes()[last], theme_, layout::NodeContentWidth(doc.GetNodes()[last], theme_, theme_.ContentWidth(kWidth))));
}

// 表示幅を渡した worker の推定は、UI スレッドで推定し直したものと同じ高さと位置になる。
TEST_F(LayoutServiceTest, WidthAwareWorkerEstimateMatchesReestimate)
{
    constexpr float kWidth = 800.0f;

    std::string md;
    for (int i = 0; i < 50; ++i) {
        md += "# H\n\n" + std::string(static_cast<size_t>(100 + i * 40), 'a') + "\n\n```\ncode\n```\n\n";
    }
    auto doc = Document::FromMarkdown(std::string{ md }, L"t.md");
    LayoutCache reestimated = layout::MakeEstimatedLayoutCache(doc.GetNodes(), theme_);
    engine_.Reestimate(doc.GetNodes(), reestimated, kWidth);
    LayoutCache width_aware = layout::MakeEstimatedLayoutCache(doc.GetNodes(), theme_, theme_.ContentWidth(kWidth));
    const auto basis = width_aware.GetEstimateBasis();
    EXPECT_FLOAT_EQ(basis.content_width, theme_.ContentWidth(kWidth));

    ASSERT_EQ(width_aware.size(), reestimated.size());
    for (size_t i = 0; i < reestimated.size(); ++i) {
        EXPECT_FLOAT_EQ(width_aware[i].height, reestimated[i].height) << i;
        EXPECT_FLOAT_EQ(width_aware.Top(i), reestimated.Top(i)) << i;
    }
}

// リロードで中身が変わらないノードの計測結果を引き継ぎ、変わったノードだけ計測し直させる。
TEST_F(LayoutServiceTest, CarryOverKeepsMeasuredEntriesOfUnchangedNodes)
{
    auto [old_nodes, old_cache] = ParseAndLayout("# Title\n\nfirst\n\nsecond\n\nthird\n", 800.0f);
    auto [new_nodes, new_cache] = ParseAndLayout("# Title\n\nfirst\n\nsecond edited\n\nthird\n", 800.0f);
    ASSERT_EQ(old_nodes.size(), 4u);
    ASSERT_EQ(new_nodes.size(), 4u);
    const float title_height = old_cache[0].height;
    new_cache = layout::MakeEstimatedLayoutCache(new_nodes, theme_);

    EXPECT_EQ(layout::CarryOverUnchanged(old_nodes, old_cache, new_nodes, new_cache, FindUnchangedEnds(old_nodes, new_nodes)), 3u);
    EXPECT_FALSE(new_cache[0].layout_dirty);
    EXPECT_FLOAT_EQ(new_cache[0].height, title_height);
    EXPECT_FALSE(new_cache[1].layout_dirty);
    EXPECT_TRUE(new_cache[2].layout_dirty) << "変わったノードは計測し直す";
    EXPECT_FALSE(new_cache[3].layout_dirty);
}

// 末尾の参照定義が変わると、前のノードはテキストが同じでもリンク先が変わるので引き継がない。
TEST_F(LayoutServiceTest, CarryOverLinkTargetChangeIsNotCarried)
{
    auto [old_nodes, old_cache] = ParseAndLayout("see [doc][r]\n\ntail\n\n[r]: https://a.example/\n", 800.0f);
    auto [new_nodes, new_cache] = ParseAndLayout("see [doc][r]\n\ntail\n\n[r]: https://b.example/\n", 800.0f);
    ASSERT_EQ(old_nodes.size(), new_nodes.size());
    new_cache = layout::MakeEstimatedLayoutCache(new_nodes, theme_);

    layout::CarryOverUnchanged(old_nodes, old_cache, new_nodes, new_cache, FindUnchangedEnds(old_nodes, new_nodes));
    EXPECT_TRUE(new_cache[0].layout_dirty);
    EXPECT_FALSE(new_cache[1].layout_dirty);
}

namespace {

constexpr std::string_view kOneDiagramMd = "# t\n\n```mermaid\ngraph TD;A-->B\n```\n\ntail\n";

// 描き終えた図を 1 つ持つ文書のレイアウト。
LayoutCache CacheWithDiagramResult(const Document& doc)
{
    auto cache = MakeUniformCache(static_cast<int>(doc.GetNodes().size()));
    auto& diagram = cache.EnsureDiagram(doc.GetDiagramNodeIndices()[0]);
    diagram.commands = &SharedStubCommandList();
    diagram.width = 100.0f;
    diagram.height = 50.0f;
    return cache;
}

} // namespace

// 図のコードを書き換えたリロードでは、旧結果と高さを仮表示として新しいノードへ引き継ぐ。
TEST(CarryOverUnchangedTest, KeepsEditedDiagramAsStale)
{
    const auto doc = Document::FromMarkdown(std::string{ kOneDiagramMd }, L"C:\\dir\\test.md");
    auto cache = CacheWithDiagramResult(doc);
    const size_t old_index = doc.GetDiagramNodeIndices()[0];
    cache.EnsureDiagram(old_index).drawing = std::make_shared<const mermaid_util::DiagramDrawing>();
    cache[old_index].height = 321.0f;

    auto next = Document::FromMarkdown(std::string{ "# t\n\n```mermaid\ngraph TD;A-->C\n```\n\ntail\n" }, L"C:\\dir\\test.md");
    auto next_cache = layout::MakeEstimatedLayoutCache(next.GetNodes(), GetLightTheme());
    const size_t new_index = next.GetDiagramNodeIndices()[0];

    EXPECT_EQ(layout::CarryOverUnchanged(doc.GetNodes(), cache, next.GetNodes(), next_cache, FindUnchangedEnds(doc.GetNodes(), next.GetNodes())), next.GetNodes().size());
    const auto* d = next_cache.FindDiagram(new_index);
    ASSERT_NE(d, nullptr);
    EXPECT_TRUE(d->HasImage());
    EXPECT_TRUE(d->stale);
    EXPECT_FALSE(d->CanExport()) << "前のコードの絵をコピー・保存させない";
    EXPECT_FLOAT_EQ(next_cache[new_index].height, 321.0f);
}

// 変わった範囲の図の数が変わったら、どの図の続きか分からないので引き継がない。
TEST(CarryOverUnchangedTest, SkipsDiagramsWhenCountChanges)
{
    const auto doc = Document::FromMarkdown(std::string{ kOneDiagramMd }, L"C:\\dir\\test.md");
    auto cache = CacheWithDiagramResult(doc);

    auto next = Document::FromMarkdown(std::string{ "# t\n\n```mermaid\ngraph TD;X-->Y\n```\n\n```mermaid\ngraph TD;A-->C\n```\n\ntail\n" }, L"C:\\dir\\test.md");
    auto next_cache = layout::MakeEstimatedLayoutCache(next.GetNodes(), GetLightTheme());

    layout::CarryOverUnchanged(doc.GetNodes(), cache, next.GetNodes(), next_cache, FindUnchangedEnds(doc.GetNodes(), next.GetNodes()));
    for (const size_t i : next.GetDiagramNodeIndices()) {
        EXPECT_EQ(next_cache.FindDiagram(i), nullptr) << "node " << i;
    }
}
