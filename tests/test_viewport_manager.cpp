#include <gtest/gtest.h>
#include "viewport_manager.h"
#include "pane_layout.h"
#include "test_helpers.h"
#include "document_types.h"

// ---- スクロールテスト ----

TEST(ViewportManagerTest, ScrollToClampsToZero)
{
    ViewportManager vm;
    const float max_scroll = MaxScroll(1000.0f, 500.0f); // max_scroll = 500
    vm.ScrollTo(-100.0f, max_scroll);
    EXPECT_FLOAT_EQ(vm.GetScrollY(), 0.0f);
}

TEST(ViewportManagerTest, ScrollToClampsToMax)
{
    ViewportManager vm;
    const float max_scroll = MaxScroll(1000.0f, 500.0f); // max_scroll = 500
    vm.ScrollTo(9999.0f, max_scroll);
    EXPECT_FLOAT_EQ(vm.GetScrollY(), 500.0f);
}

TEST(ViewportManagerTest, ClampScrollClampsExistingScroll)
{
    ViewportManager vm;
    vm.ScrollTo(600.0f, MaxScroll(1000.0f, 200.0f));
    vm.SetScrollTarget(1, 0.0f);
    EXPECT_FLOAT_EQ(vm.GetScrollY(), 600.0f);

    // コンテンツを縮小して最大値が現在のスクロール以下になるようにする
    vm.ClampScroll(MaxScroll(500.0f, 200.0f));
    EXPECT_FLOAT_EQ(vm.GetScrollY(), 300.0f);
    EXPECT_TRUE(vm.GetScrollTarget().IsValid());
}

TEST(ViewportManagerTest, MaxScrollNoNegative)
{
    // コンテンツがビューポートより小さい
    EXPECT_FLOAT_EQ(MaxScroll(100.0f, 500.0f), 0.0f);
}

TEST(ViewportManagerTest, DirectScrollByMovesPosition)
{
    ViewportManager vm;
    const float max_scroll = MaxScroll(10000.0f, 200.0f);

    vm.DirectScrollBy(100.0f, max_scroll);
    EXPECT_FLOAT_EQ(vm.GetScrollY(), 100.0f);
}

TEST(ViewportManagerTest, DirectScrollByClampsToMax)
{
    ViewportManager vm;
    const float max_scroll = MaxScroll(300.0f, 200.0f); // max = 100
    vm.DirectScrollBy(9999.0f, max_scroll);
    EXPECT_FLOAT_EQ(vm.GetScrollY(), 100.0f);
}

TEST(ViewportManagerTest, DirectScrollByClampsToZero)
{
    ViewportManager vm;
    const float max_scroll = MaxScroll(1000.0f, 200.0f);
    vm.DirectScrollBy(50.0f, max_scroll);
    vm.DirectScrollBy(-200.0f, max_scroll);
    EXPECT_FLOAT_EQ(vm.GetScrollY(), 0.0f);
}

TEST(ViewportManagerTest, DirectScrollByAccumulates)
{
    // タッチパッドの連続入力をシミュレーション
    ViewportManager vm;
    const float max_scroll = MaxScroll(100000.0f, 200.0f);

    float total = 0.0f;
    for (int i = 0; i < 30; ++i) {
        vm.DirectScrollBy(24.0f, max_scroll);
        total += 24.0f;
    }

    EXPECT_FLOAT_EQ(vm.GetScrollY(), total);
}

// ---- FindFirstVisibleNode テスト ----

// 位置からの探索は FindFirstVisibleNodeIndex のテストが見る。ここは scroll_y を渡すことと、無いときの -1。
TEST(ViewportManagerTest, FindFirstVisibleNodeUsesScrollAndReportsNone)
{
    ViewportManager vm;
    auto cache = MakeUniformCache(10, 50.0f);
    vm.ScrollTo(120.0f, MaxScroll(500.0f, 200.0f));
    EXPECT_EQ(vm.FindFirstVisibleNode(cache), 2);
    EXPECT_EQ(vm.FindFirstVisibleNode(LayoutCache{}), -1);
}

// ---- ScrollTarget テスト ----

TEST(ViewportManagerTest, ScrollTargetAppliesNewYPosition)
{
    // レイアウト変更でターゲットノードの y_position が動くと scroll_y も追従する
    ViewportManager vm;
    auto cache = MakeUniformCache(5, 100.0f);
    const float max_scroll = MaxScroll(500.0f, 200.0f);
    vm.ScrollTo(100.0f, max_scroll);

    float offset = vm.GetScrollY() - cache.Top(1); // 0
    vm.SetScrollTarget(1, offset);

    // レイアウト変更をシミュレーション: ノード1を30下にシフト
    cache.ShiftTops(1, 30.0f);

    vm.ApplyScrollTarget(cache);
    EXPECT_FLOAT_EQ(vm.GetScrollY(), 130.0f);
}

TEST(ViewportManagerTest, ApplyScrollTargetIsNoOpWhenInvalid)
{
    ViewportManager vm;
    auto cache = MakeUniformCache(3, 100.0f);
    const float max_scroll = MaxScroll(300.0f, 100.0f);
    vm.ScrollTo(50.0f, max_scroll);
    float before = vm.GetScrollY();
    // target 未設定なので何もしない
    EXPECT_FALSE(vm.GetScrollTarget().IsValid());
    vm.ApplyScrollTarget(cache);
    EXPECT_FLOAT_EQ(vm.GetScrollY(), before);
}

TEST(ViewportManagerTest, ScrollToInvalidatesScrollTarget)
{
    ViewportManager vm;
    vm.SetScrollTarget(2, 10.0f);
    EXPECT_TRUE(vm.GetScrollTarget().IsValid());
    vm.ScrollTo(100.0f, 0.0f);
    EXPECT_FALSE(vm.GetScrollTarget().IsValid());
}

TEST(ViewportManagerTest, DirectScrollByInvalidatesScrollTarget)
{
    ViewportManager vm;
    const float max_scroll = MaxScroll(1000.0f, 200.0f);
    vm.SetScrollTarget(2, 10.0f);
    vm.DirectScrollBy(50.0f, max_scroll);
    EXPECT_FALSE(vm.GetScrollTarget().IsValid());
}

TEST(ViewportManagerTest, EnsureScrollTargetSynthesizesFromVisibleAnchor)
{
    ViewportManager vm;
    auto cache = MakeUniformCache(5, 100.0f);
    const float max_scroll = MaxScroll(500.0f, 200.0f);
    vm.ScrollTo(150.0f, max_scroll); // ノード1の途中（scroll_y=150, cache[1].y=100）

    vm.EnsureScrollTarget(cache);
    ASSERT_TRUE(vm.GetScrollTarget().IsValid());
    EXPECT_EQ(vm.GetScrollTarget().node, 1);
    EXPECT_FLOAT_EQ(vm.GetScrollTarget().offset, 50.0f);
}

TEST(ViewportManagerTest, EnsureScrollTargetPreservesExistingTarget)
{
    ViewportManager vm;
    auto cache = MakeUniformCache(5, 100.0f);
    vm.SetScrollY(150.0f);
    vm.SetScrollTarget(3, 25.0f);

    vm.EnsureScrollTarget(cache);
    // 既存ターゲットは上書きされない
    EXPECT_EQ(vm.GetScrollTarget().node, 3);
    EXPECT_FLOAT_EQ(vm.GetScrollTarget().offset, 25.0f);
}

// ---- 選択テスト ----

TEST(ViewportManagerTest, ClearSelection)
{
    ViewportManager vm;
    vm.SetSelection(TextSelection::MakeOrdered(0, 0, 5, 20));

    vm.ClearSelection();
    EXPECT_FALSE(vm.GetSelection().active());
}

TEST(ViewportManagerTest, SelectAll)
{
    std::vector<Node> nodes(3);
    nodes[0].SetText("hello");
    nodes[1].SetText("world");
    nodes[2].SetText("end");

    ViewportManager vm;
    vm.SelectAll(nodes);

    EXPECT_TRUE(vm.GetSelection().active());
    EXPECT_EQ(vm.GetSelection().start_node, 0);
    EXPECT_EQ(vm.GetSelection().start_pos, 0u);
    EXPECT_EQ(vm.GetSelection().end_node, 2);
    EXPECT_EQ(vm.GetSelection().end_pos, 3u);
}

TEST(ViewportManagerTest, SelectAllEmpty)
{
    std::vector<Node> nodes;
    ViewportManager vm;
    vm.SelectAll(nodes);
    EXPECT_FALSE(vm.GetSelection().active());
}

// 最終ノードが Table のときも concat_text (= GetText) の末尾を終端にする。
TEST(ViewportManagerTest, SelectAllLastNodeIsTable)
{
    std::vector<Node> nodes;
    nodes.push_back(MakeTextNode("hello"));
    nodes.push_back(MakeTableNode({ { "cell0", "cell1" } }));

    ViewportManager vm;
    vm.SelectAll(nodes);

    EXPECT_TRUE(vm.GetSelection().active());
    EXPECT_EQ(vm.GetSelection().start_node, 0);
    EXPECT_EQ(vm.GetSelection().start_pos, 0u);
    EXPECT_EQ(vm.GetSelection().end_node, 1);
    EXPECT_EQ(vm.GetSelection().end_pos, static_cast<uint32_t>(nodes[1].table_data()->concat_text.size()));
}

// ---- ズームテスト ----

TEST(ViewportManagerTest, ZoomInAdvancesIndex)
{
    ViewportManager vm; // starts at ZOOM_DEFAULT_INDEX (1.0)
    EXPECT_TRUE(vm.Zoom().Step(ZoomDirection::In));
    EXPECT_EQ(vm.Zoom().Index(), ZOOM_DEFAULT_INDEX + 1);
    EXPECT_GT(vm.Zoom().Value(), 1.0f);
}

TEST(ViewportManagerTest, ZoomOutDecrementsIndex)
{
    ViewportManager vm;
    EXPECT_TRUE(vm.Zoom().Step(ZoomDirection::Out));
    EXPECT_EQ(vm.Zoom().Index(), ZOOM_DEFAULT_INDEX - 1);
    EXPECT_LT(vm.Zoom().Value(), 1.0f);
    EXPECT_GT(vm.Zoom().Value(), 0.0f);
}

TEST(ViewportManagerTest, ZoomInAtMaxReturnsFalse)
{
    ViewportManager vm;
    // 最大までズーム
    for (int i = 0; i < 50; ++i)
        vm.Zoom().Step(ZoomDirection::In);
    EXPECT_FALSE(vm.Zoom().Step(ZoomDirection::In));
    EXPECT_EQ(vm.Zoom().Index(), ZOOM_STEP_COUNT - 1);
}

TEST(ViewportManagerTest, ZoomOutAtMinReturnsFalse)
{
    ViewportManager vm;
    // 最小までズーム
    for (int i = 0; i < 50; ++i)
        vm.Zoom().Step(ZoomDirection::Out);
    EXPECT_FALSE(vm.Zoom().Step(ZoomDirection::Out));
    EXPECT_EQ(vm.Zoom().Index(), 0);
}

TEST(ViewportManagerTest, ZoomResetFromNonDefault)
{
    ViewportManager vm;
    vm.Zoom().Step(ZoomDirection::In);
    vm.Zoom().Step(ZoomDirection::In);
    EXPECT_TRUE(vm.Zoom().Step(ZoomDirection::Reset));
    EXPECT_EQ(vm.Zoom().Index(), ZOOM_DEFAULT_INDEX);
    EXPECT_FLOAT_EQ(vm.Zoom().Value(), 1.0f);
}

TEST(ViewportManagerTest, ZoomResetAlreadyDefault)
{
    ViewportManager vm;
    EXPECT_FALSE(vm.Zoom().Step(ZoomDirection::Reset)); // 変更不要
    EXPECT_EQ(vm.Zoom().Index(), ZOOM_DEFAULT_INDEX);
}

TEST(ViewportManagerTest, SetZoomIndexClampsBelowZero)
{
    ViewportManager vm;
    vm.Zoom().SetIndex(-5);
    EXPECT_EQ(vm.Zoom().Index(), 0);
    EXPECT_FLOAT_EQ(vm.Zoom().Value(), ZOOM_STEPS[0]);
}

TEST(ViewportManagerTest, SetZoomIndexClampsAboveMax)
{
    ViewportManager vm;
    vm.Zoom().SetIndex(ZOOM_STEP_COUNT + 100);
    EXPECT_EQ(vm.Zoom().Index(), ZOOM_STEP_COUNT - 1);
    EXPECT_FLOAT_EQ(vm.Zoom().Value(), ZOOM_STEPS[ZOOM_STEP_COUNT - 1]);
}

TEST(ViewportManagerTest, SetZoomIndexAcceptsValidValues)
{
    ViewportManager vm;
    vm.Zoom().SetIndex(3);
    EXPECT_EQ(vm.Zoom().Index(), 3);
    EXPECT_FLOAT_EQ(vm.Zoom().Value(), ZOOM_STEPS[3]);
}

// ---- バグ #17: scroll_y の非負クランプ ----

TEST(ViewportManagerTest, ApplyScrollTargetClampsNegative)
{
    ViewportManager vm;
    auto cache = MakeUniformCache(5, 100.0f);
    const float max_scroll = MaxScroll(500.0f, 200.0f);
    vm.ScrollTo(50.0f, max_scroll);

    // target 設定: scroll_y - y_position[2] = 50 - 200 = -150
    vm.SetScrollTarget(2, vm.GetScrollY() - cache.Top(2));

    // ノード2を上方に300シフトするレイアウト変更をシミュレーション
    // （例: 上にある大きなノードが削除された場合）
    cache.SetTop(2, 0.0f);

    vm.ApplyScrollTarget(cache);

    // scroll_yは-150ではなく0にクランプされるべき
    EXPECT_GE(vm.GetScrollY(), 0.0f);
}

TEST(ViewportManagerTest, ApplyScrollTargetPositiveShiftOk)
{
    ViewportManager vm;
    auto cache = MakeUniformCache(5, 100.0f);
    const float max_scroll = MaxScroll(500.0f, 200.0f);
    vm.ScrollTo(100.0f, max_scroll);

    vm.SetScrollTarget(1, vm.GetScrollY() - cache.Top(1));
    cache.SetTop(1, 150.0f); // shifted down by 50

    vm.ApplyScrollTarget(cache);
    EXPECT_FLOAT_EQ(vm.GetScrollY(), 150.0f);
}

// ---- セッション復元テスト（ノードベーススクロール復元） ----

// ノード基準の位置を保存して当て直すと元の位置へ戻り、画像の読み込みなどで前のノードの高さが変わっても同じノードを指す。
TEST(ViewportManagerTest, ScrollRestoreFollowsNodeAcrossHeightChange)
{
    constexpr float kShift = 150.0f;
    struct Case {
        float scroll;
        int node;
        float offset;
    };
    for (const Case c : { Case{ 0.0f, 0, 0.0f }, Case{ 125.0f, 2, 25.0f }, Case{ 200.0f, 4, 0.0f }, Case{ 300.0f, 6, 0.0f } }) {
        SCOPED_TRACE(c.scroll);
        auto cache = MakeUniformCache(10, 50.0f);
        ViewportManager vm;
        vm.ScrollTo(c.scroll, MaxScroll(500.0f, 200.0f));

        const ScrollTarget saved = vm.CurrentTarget(cache);
        EXPECT_EQ(saved.node, c.node);
        EXPECT_FLOAT_EQ(saved.offset, c.offset);

        ViewportManager restored;
        restored.SetScrollTarget(saved.node, saved.offset);
        restored.ApplyScrollTarget(cache);
        EXPECT_FLOAT_EQ(restored.GetScrollY(), c.scroll);

        cache.ShiftTops(1, kShift);
        restored.ApplyScrollTarget(cache);
        EXPECT_FLOAT_EQ(restored.GetScrollY(), c.scroll + (c.node >= 1 ? kShift : 0.0f));
    }
}

// ---- 位置未確定キャッシュ ----

// 作った直後のキャッシュから見ている位置を合成すると誤ったノードへ飛ぶので、合成しない。
TEST(ViewportManagerTest, EnsureScrollTargetSkipsFreshCache)
{
    LayoutCache cache(5);
    ViewportManager vm;

    EXPECT_EQ(vm.FindFirstVisibleNode(cache), -1);
    vm.EnsureScrollTarget(cache);
    EXPECT_FALSE(vm.GetScrollTarget().IsValid());
}

// Step は向きごとの操作と同じで、CanStep は段が動くかと一致する。
TEST(ZoomLevelTest, StepMatchesCanStep)
{
    for (const int index : { 0, ZOOM_DEFAULT_INDEX, ZOOM_STEP_COUNT - 1 }) {
        for (const auto direction : { ZoomDirection::In, ZoomDirection::Out, ZoomDirection::Reset }) {
            SCOPED_TRACE(index);
            ZoomLevel zoom;
            zoom.SetIndex(index);
            const bool can = zoom.CanStep(direction);
            EXPECT_EQ(zoom.Step(direction), can);
            EXPECT_EQ(zoom.Index() != index, can);
        }
    }
}

TEST(ZoomLevelTest, PercentRoundsStepValue)
{
    ZoomLevel zoom;
    EXPECT_EQ(zoom.Percent(), 100);
    zoom.SetIndex(1);
    EXPECT_EQ(zoom.Percent(), 33);
    zoom.SetIndex(3);
    EXPECT_EQ(zoom.Percent(), 67);
}
