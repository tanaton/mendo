// 部分レイアウト・ダーティ処理・エビクトを乱数の操作列で混ぜ、不変条件と
// 全再計算との一致を確かめるモデルベーステスト。
#include <gtest/gtest.h>
#include <algorithm>
#include <cmath>
#include <deque>
#include <memory_resource>
#include <random>
#include <string>
#include "layout.h"
#include "layout_invariants.h"
#include "mock_text_measurer.h"
#include "parser.h"
#include "task_scheduler.h"
#include "test_helpers.h"

namespace {

constexpr float kYEps = 0.05f;
// LayoutEngine::ComputeLayout が再計測を省く幅変化の上限。
constexpr float kWidthHysteresis = LayoutEngine::kWidthChangeThreshold;

// 計測したエントリに目印 (行追跡の無い table_layout) を残す。EvictEntryLayout はこれを
// 破棄するので、keep 範囲外に目印が残っていれば解放漏れと分かる。
class MarkingMeasurer : public MockTextMeasurer {
public:
    void MeasureNode(Node& node, NodeLayoutEntry& entry, float max_width,
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
        nodes_ = ParseMarkdown(sources_.emplace_back(MakeRandomMarkdown(rng, blocks))).nodes;
        cache_.Reset(nodes_.size());
        EstimateNodeHeights(nodes_, cache_, theme_);
        width_ = 800.0f;
        scroll_ = 0.0f;
        engine_.ComputeLayout(nodes_, cache_, width_, scroll_, scroll_ + kViewportHeight);
    }

    float MaxScroll() const
    {
        return std::max(0.0f, ComputeTotalContentHeight(cache_, nodes_.size(), theme_.margin_bottom) - kViewportHeight);
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
            const float drift = std::abs(cache_[i].cached_width - NodeWidth(i));
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
            engine_.ComputeLayout(nodes_, cache_, width_, scroll_, scroll_ + kViewportHeight);
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
            keep_out = ComputeVisibleNodeRange(cache_, nodes_.size(), scroll_ - buffer, scroll_ + kViewportHeight + buffer);
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
        engine_.ComputeLayout(nodes_, cache_, width_, scroll_, scroll_ + kViewportHeight);
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
                ASSERT_FALSE(cache_[i].has_table_layout()) << "keep 範囲外 i=" << i << " の layout が残っている";
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
            measurer_.MeasureNode(nodes_[i], remeasured, cache_[i].cached_width);
            ASSERT_FLOAT_EQ(cache_[i].height, remeasured.height) << "i=" << i;
        }

        // ヒステリシスを超える幅変更の後は、新規に全レイアウトした結果と一致する。
        width_ += 10.0f;
        engine_.ComputeLayout(nodes_, cache_, width_, scroll_, scroll_ + kViewportHeight);
        Settle();

        LayoutEngine ref_engine;
        ASSERT_TRUE(ref_engine.Init(&measurer_, theme_));
        LayoutCache ref;
        ref.Resize(nodes_.size());
        EstimateNodeHeights(nodes_, ref, theme_);
        ref_engine.ComputeLayout(nodes_, ref, width_);
        for (size_t i = 0; i < nodes_.size(); ++i) {
            ASSERT_NEAR(cache_[i].height, ref[i].height, kYEps) << "i=" << i;
            ASSERT_NEAR(cache_.Top(i), ref.Top(i), kYEps) << "i=" << i;
        }
    }
}
