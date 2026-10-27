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
    EXPECT_TRUE(vm.HasScrollTarget());
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

TEST(ViewportManagerTest, FindFirstVisibleNodeStart)
{
    ViewportManager vm;
    auto cache = MakeUniformCache(10, 50.0f);
    const float max_scroll = MaxScroll(500.0f, 200.0f);
    vm.ScrollTo(0.0f, max_scroll);
    EXPECT_EQ(vm.FindFirstVisibleNode(cache), 0);
}

TEST(ViewportManagerTest, FindFirstVisibleNodeMiddle)
{
    ViewportManager vm;
    auto cache = MakeUniformCache(10, 50.0f);
    const float max_scroll = MaxScroll(500.0f, 200.0f);
    vm.ScrollTo(120.0f, max_scroll); // y=100のノード(インデックス2)が見えるはず
    EXPECT_EQ(vm.FindFirstVisibleNode(cache), 2);
}

TEST(ViewportManagerTest, FindFirstVisibleNodeEnd)
{
    ViewportManager vm;
    auto cache = MakeUniformCache(10, 50.0f);
    const float max_scroll = MaxScroll(500.0f, 200.0f);
    vm.ScrollTo(300.0f, max_scroll); // max_scroll=300, node 6 starts at y=300
    int idx = vm.FindFirstVisibleNode(cache);
    EXPECT_GE(idx, 6);
}

TEST(ViewportManagerTest, FindFirstVisibleNodeEmpty)
{
    ViewportManager vm;
    LayoutCache cache;
    EXPECT_EQ(vm.FindFirstVisibleNode(cache), -1);
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
    cache.SetTop(1, 130.0f);
    cache.SetTop(2, 230.0f);
    cache.SetTop(3, 330.0f);
    cache.SetTop(4, 430.0f);

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
    EXPECT_FALSE(vm.HasScrollTarget());
    vm.ApplyScrollTarget(cache);
    EXPECT_FLOAT_EQ(vm.GetScrollY(), before);
}

TEST(ViewportManagerTest, ScrollToInvalidatesScrollTarget)
{
    ViewportManager vm;
    vm.SetScrollTarget(2, 10.0f);
    EXPECT_TRUE(vm.HasScrollTarget());
    vm.ScrollTo(100.0f, 0.0f);
    EXPECT_FALSE(vm.HasScrollTarget());
}

TEST(ViewportManagerTest, DirectScrollByInvalidatesScrollTarget)
{
    ViewportManager vm;
    const float max_scroll = MaxScroll(1000.0f, 200.0f);
    vm.SetScrollTarget(2, 10.0f);
    vm.DirectScrollBy(50.0f, max_scroll);
    EXPECT_FALSE(vm.HasScrollTarget());
}

TEST(ViewportManagerTest, EnsureScrollTargetSynthesizesFromVisibleAnchor)
{
    ViewportManager vm;
    auto cache = MakeUniformCache(5, 100.0f);
    const float max_scroll = MaxScroll(500.0f, 200.0f);
    vm.ScrollTo(150.0f, max_scroll); // ノード1の途中（scroll_y=150, cache[1].y=100）

    vm.EnsureScrollTarget(cache);
    ASSERT_TRUE(vm.HasScrollTarget());
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
    nodes[0].SetText("hello", 0);
    nodes[1].SetText("world", 0);
    nodes[2].SetText("end", 0);

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

TEST(ViewportManagerTest, ScrollRestoreRoundTrip)
{
    // ノード+オフセットで保存→復元すると元のスクロール位置を再現できる
    ViewportManager vm;
    auto cache = MakeUniformCache(10, 50.0f);           // total=500
    const float max_scroll = MaxScroll(500.0f, 200.0f); // max_scroll=300
    vm.ScrollTo(125.0f, max_scroll);                    // ノード2の途中

    int saved_node = vm.FindFirstVisibleNode(cache);
    float saved_offset = vm.GetScrollY() - cache.Top(saved_node);

    // 復元
    float restored = cache.Top(saved_node) + saved_offset;
    EXPECT_FLOAT_EQ(restored, 125.0f);
}

TEST(ViewportManagerTest, ScrollRestoreResilientToHeightChange)
{
    // 画像未読み込み→読み込み後でレイアウトが変わっても正しいノードを指す
    ViewportManager vm;
    auto cache = MakeUniformCache(10, 50.0f);
    const float max_scroll = MaxScroll(500.0f, 200.0f);
    vm.ScrollTo(200.0f, max_scroll); // ノード4の先頭

    int saved_node = vm.FindFirstVisibleNode(cache);
    float saved_offset = vm.GetScrollY() - cache.Top(saved_node);
    EXPECT_EQ(saved_node, 4);
    EXPECT_FLOAT_EQ(saved_offset, 0.0f);

    // レイアウト変更: ノード1の高さが200に増加（画像読み込みシミュレーション）
    LayoutCache new_cache(10);
    float y = 0.0f;
    for (int i = 0; i < 10; ++i) {
        new_cache.SetTop(i, y);
        new_cache[i].height = (i == 1) ? 200.0f : 50.0f;
        y += new_cache[i].height;
    }
    // ノード4の新しいy位置: 50 + 200 + 50 + 50 = 350
    EXPECT_FLOAT_EQ(new_cache.Top(saved_node), 350.0f);

    // ノードベース復元: 正しいノードの先頭にスクロール
    float restored = new_cache.Top(saved_node) + saved_offset;
    EXPECT_FLOAT_EQ(restored, 350.0f);
}

TEST(ViewportManagerTest, ScrollRestoreWithPartialNodeVisibility)
{
    // ノードの途中で保存した場合、オフセットがノード内の位置を再現する
    ViewportManager vm;
    auto cache = MakeUniformCache(10, 100.0f); // total=1000
    const float max_scroll = MaxScroll(1000.0f, 300.0f);
    vm.ScrollTo(350.0f, max_scroll); // ノード3(y=300)の途中

    int saved_node = vm.FindFirstVisibleNode(cache);
    float saved_offset = vm.GetScrollY() - cache.Top(saved_node);
    EXPECT_EQ(saved_node, 3);
    EXPECT_FLOAT_EQ(saved_offset, 50.0f);

    float restored = cache.Top(saved_node) + saved_offset;
    EXPECT_FLOAT_EQ(restored, 350.0f);
}

TEST(ViewportManagerTest, ScrollRestoreNodeClampedToSize)
{
    // 保存されたノードインデックスが現在のノード数を超える場合は最終ノードに制限
    auto cache = MakeUniformCache(5, 100.0f);
    int saved_node = 8; // 5個しかないので超過
    int clamped = std::min(saved_node, static_cast<int>(cache.size()) - 1);
    EXPECT_EQ(clamped, 4);

    float restored = std::max(0.0f, cache.Top(clamped) + 0.0f);
    EXPECT_FLOAT_EQ(restored, 400.0f);
}

TEST(ViewportManagerTest, ScrollRestoreAtDocumentStart)
{
    ViewportManager vm;
    auto cache = MakeUniformCache(10, 50.0f);
    const float max_scroll = MaxScroll(500.0f, 200.0f);
    vm.ScrollTo(0.0f, max_scroll);

    int saved_node = vm.FindFirstVisibleNode(cache);
    float saved_offset = vm.GetScrollY() - cache.Top(saved_node);

    EXPECT_EQ(saved_node, 0);
    EXPECT_FLOAT_EQ(saved_offset, 0.0f);

    float restored = cache.Top(saved_node) + saved_offset;
    EXPECT_FLOAT_EQ(restored, 0.0f);
}

TEST(ViewportManagerTest, ScrollRestoreAtDocumentEnd)
{
    ViewportManager vm;
    auto cache = MakeUniformCache(10, 50.0f);           // total=500
    const float max_scroll = MaxScroll(500.0f, 200.0f); // max_scroll=300
    vm.ScrollTo(300.0f, max_scroll);                    // 最下端

    int saved_node = vm.FindFirstVisibleNode(cache);
    float saved_offset = vm.GetScrollY() - cache.Top(saved_node);

    float restored = cache.Top(saved_node) + saved_offset;
    EXPECT_FLOAT_EQ(restored, 300.0f);
}

TEST(ViewportManagerTest, ScrollRestoreTargetCompensationAfterHeightChange)
{
    // ノードベース復元後に画像読み込みで target 再評価が正しく機能する
    ViewportManager vm;
    auto cache = MakeUniformCache(10, 50.0f);

    // ノード5(y=250)付近にスクロール復元
    int restore_node = 5;
    float restore_offset = 10.0f;
    vm.SetScrollTarget(restore_node, restore_offset);
    vm.ApplyScrollTarget(cache);
    EXPECT_FLOAT_EQ(vm.GetScrollY(), 260.0f);

    // ノード2の高さが100増加（画像読み込み）→ ノード5が下にシフト
    for (int i = 2; i < 10; ++i) {
        cache.SetTop(i, cache.Top(i) + 100.0f);
    }
    cache[2].height = 150.0f;

    // target 再評価
    vm.ApplyScrollTarget(cache);
    // scroll_yが100増加して360になるべき（ノード5が350に移動+オフセット10）
    EXPECT_FLOAT_EQ(vm.GetScrollY(), 360.0f);
}

// ---- ClampAndDetach / 位置未確定キャッシュ ----

TEST(ViewportManagerTest, ClampAndDetachClampsAndClearsTarget)
{
    ViewportManager vm;
    const float max_scroll = MaxScroll(1000.0f, 500.0f);
    vm.SetScrollY(800.0f);
    vm.SetScrollTarget(3, 10.0f);

    vm.ClampAndDetach(max_scroll);

    EXPECT_FLOAT_EQ(vm.GetScrollY(), 500.0f);
    EXPECT_FALSE(vm.HasScrollTarget());
}

// 作った直後のキャッシュから見ている位置を合成すると誤ったノードへ飛ぶので、合成しない。
TEST(ViewportManagerTest, EnsureScrollTargetSkipsFreshCache)
{
    LayoutCache cache(5);
    ViewportManager vm;

    EXPECT_EQ(vm.FindFirstVisibleNode(cache), -1);
    vm.EnsureScrollTarget(cache);
    EXPECT_FALSE(vm.HasScrollTarget());
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
