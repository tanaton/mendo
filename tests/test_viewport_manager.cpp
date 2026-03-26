#include <gtest/gtest.h>
#include <memory_resource>
#include "viewport_manager.h"
#include "layout_cache.h"
#include "types.h"

// ヘルパー: 等間隔に配置されたノードで単純なLayoutCacheを構築する。
static LayoutCache MakeTestCache(int count, float node_height = 50.0f) {
    LayoutCache cache;
    cache.Resize(count);
    float y = 0.0f;
    for (int i = 0; i < count; ++i) {
        cache[i].y_position = y;
        cache[i].height = node_height;
        y += node_height;
    }
    return cache;
}

// ---- スクロールテスト ----

TEST(ViewportManagerTest, ScrollToClampsToZero) {
    ViewportManager vm;
    vm.SyncMaxScroll(1000.0f, 500.0f); // max_scroll = 500
    vm.ScrollTo(-100.0f);
    EXPECT_FLOAT_EQ(vm.GetScrollY(), 0.0f);
}

TEST(ViewportManagerTest, ScrollToClampsToMax) {
    ViewportManager vm;
    vm.SyncMaxScroll(1000.0f, 500.0f); // max_scroll = 500
    vm.ScrollTo(9999.0f);
    EXPECT_FLOAT_EQ(vm.GetScrollY(), 500.0f);
}

TEST(ViewportManagerTest, SyncMaxScrollClampsExistingScroll) {
    ViewportManager vm;
    vm.SyncMaxScroll(1000.0f, 200.0f); // max = 800
    vm.ScrollTo(600.0f);
    EXPECT_FLOAT_EQ(vm.GetScrollY(), 600.0f);

    // コンテンツを縮小して最大値が現在のスクロール以下になるようにする
    vm.SyncMaxScroll(500.0f, 200.0f); // max = 300
    EXPECT_FLOAT_EQ(vm.GetMaxScroll(), 300.0f);
    EXPECT_FLOAT_EQ(vm.GetScrollY(), 300.0f);
}

TEST(ViewportManagerTest, SyncMaxScrollNoNegative) {
    ViewportManager vm;
    vm.SyncMaxScroll(100.0f, 500.0f); // コンテンツがビューポートより小さい
    EXPECT_FLOAT_EQ(vm.GetMaxScroll(), 0.0f);
}

// ---- スムーススクロールテスト ----

TEST(ViewportManagerTest, SmoothScrollActivates) {
    ViewportManager vm;
    vm.SyncMaxScroll(1000.0f, 200.0f);
    EXPECT_FALSE(vm.IsSmoothScrolling());

    vm.SmoothScrollBy(100.0f);
    EXPECT_TRUE(vm.IsSmoothScrolling());
    EXPECT_FLOAT_EQ(vm.GetScrollTarget(), 100.0f);
}

TEST(ViewportManagerTest, SmoothScrollConverges) {
    ViewportManager vm;
    vm.SyncMaxScroll(10000.0f, 200.0f);
    vm.SmoothScrollBy(400.0f);

    // 収束に十分なフレーム数を実行
    for (int i = 0; i < 200; ++i) {
        bool active = vm.UpdateSmoothScroll();
        if (!active) break;
    }

    EXPECT_FALSE(vm.IsSmoothScrolling());
    EXPECT_FLOAT_EQ(vm.GetScrollY(), 400.0f);
}

TEST(ViewportManagerTest, SmoothScrollClampedTarget) {
    ViewportManager vm;
    vm.SyncMaxScroll(300.0f, 200.0f); // max = 100
    vm.SmoothScrollBy(9999.0f);
    EXPECT_FLOAT_EQ(vm.GetScrollTarget(), 100.0f);
}

TEST(ViewportManagerTest, StopSmoothScroll) {
    ViewportManager vm;
    vm.SyncMaxScroll(1000.0f, 200.0f);
    vm.SmoothScrollBy(200.0f);
    vm.UpdateSmoothScroll();

    vm.StopSmoothScroll();
    EXPECT_FALSE(vm.IsSmoothScrolling());
    EXPECT_FLOAT_EQ(vm.GetScrollY(), vm.GetScrollTarget());
}

// ---- デルタタイム対応スムーススクロールテスト ----

TEST(ViewportManagerTest, SmoothScrollWithDeltaTimeConverges) {
    ViewportManager vm;
    vm.SyncMaxScroll(10000.0f, 200.0f);
    vm.SmoothScrollBy(400.0f);

    // 7ms刻み（≈144fps）で収束を確認
    for (int i = 0; i < 500; ++i) {
        bool active = vm.UpdateSmoothScroll(7.0f);
        if (!active) {
            break;
        }
    }

    EXPECT_FALSE(vm.IsSmoothScrolling());
    EXPECT_FLOAT_EQ(vm.GetScrollY(), 400.0f);
}

TEST(ViewportManagerTest, SmoothScrollFrameRateIndependence) {
    // 同じスクロール量を異なるフレームレートで実行し、同一時間後のスクロール位置が近いことを確認
    auto simulate = [](float dt_ms, int frames) {
        ViewportManager vm;
        vm.SyncMaxScroll(10000.0f, 200.0f);
        vm.SmoothScrollBy(1000.0f);
        for (int i = 0; i < frames; ++i) {
            vm.UpdateSmoothScroll(dt_ms);
        }
        return vm.GetScrollY();
    };

    // 160ms分のシミュレーション: 60fps(10フレーム) vs 144fps(23フレーム)
    float pos_60fps = simulate(16.0f, 10);
    float pos_144fps = simulate(6.957f, 23);  // 23 * 6.957 ≈ 160ms

    // 同一時間経過後のスクロール位置は近似すべき（誤差5%以内）
    EXPECT_NEAR(pos_60fps, pos_144fps, 1000.0f * 0.05f);
}

TEST(ViewportManagerTest, SmoothScrollReferenceDtMatchesLegacy) {
    // 基準フレーム時間(16ms)での呼び出しは引数なし版と同一結果
    ViewportManager vm1, vm2;
    vm1.SyncMaxScroll(10000.0f, 200.0f);
    vm2.SyncMaxScroll(10000.0f, 200.0f);
    vm1.SmoothScrollBy(500.0f);
    vm2.SmoothScrollBy(500.0f);

    for (int i = 0; i < 30; ++i) {
        vm1.UpdateSmoothScroll();
        vm2.UpdateSmoothScroll(ViewportManager::SCROLL_REFERENCE_DT);
        EXPECT_NEAR(vm1.GetScrollY(), vm2.GetScrollY(), 1e-3f);
    }
}

TEST(ViewportManagerTest, SmoothScrollLargeDeltaTimeClamped) {
    // 大きなデルタタイムでもクラッシュや発散しない
    ViewportManager vm;
    vm.SyncMaxScroll(10000.0f, 200.0f);
    vm.SmoothScrollBy(500.0f);

    vm.UpdateSmoothScroll(1000.0f); // 1秒分の大きなデルタ
    EXPECT_GE(vm.GetScrollY(), 0.0f);
    EXPECT_LE(vm.GetScrollY(), 10000.0f);
}

// ---- FindFirstVisibleNode テスト ----

TEST(ViewportManagerTest, FindFirstVisibleNodeStart) {
    ViewportManager vm;
    auto cache = MakeTestCache(10, 50.0f);
    vm.SyncMaxScroll(500.0f, 200.0f);
    vm.ScrollTo(0.0f);
    EXPECT_EQ(vm.FindFirstVisibleNode(cache, 10), 0);
}

TEST(ViewportManagerTest, FindFirstVisibleNodeMiddle) {
    ViewportManager vm;
    auto cache = MakeTestCache(10, 50.0f);
    vm.SyncMaxScroll(500.0f, 200.0f);
    vm.ScrollTo(120.0f); // y=100のノード(インデックス2)が見えるはず
    EXPECT_EQ(vm.FindFirstVisibleNode(cache, 10), 2);
}

TEST(ViewportManagerTest, FindFirstVisibleNodeEnd) {
    ViewportManager vm;
    auto cache = MakeTestCache(10, 50.0f);
    vm.SyncMaxScroll(500.0f, 200.0f);
    vm.ScrollTo(300.0f); // max_scroll=300, node 6 starts at y=300
    int idx = vm.FindFirstVisibleNode(cache, 10);
    EXPECT_GE(idx, 6);
}

TEST(ViewportManagerTest, FindFirstVisibleNodeEmpty) {
    ViewportManager vm;
    LayoutCache cache;
    EXPECT_EQ(vm.FindFirstVisibleNode(cache, 0), -1);
}

// ---- AnchorCompensateScroll テスト ----

TEST(ViewportManagerTest, AnchorCompensateScroll) {
    ViewportManager vm;
    auto cache = MakeTestCache(5, 100.0f);
    vm.SyncMaxScroll(500.0f, 200.0f);
    vm.ScrollTo(100.0f);

    float y_before = cache[1].y_position; // 100.0f
    // レイアウト変更をシミュレーション: ノード1を30下にシフト
    cache[1].y_position = 130.0f;
    cache[2].y_position = 230.0f;
    cache[3].y_position = 330.0f;
    cache[4].y_position = 430.0f;

    vm.AnchorCompensateScroll(1, y_before, cache);
    EXPECT_FLOAT_EQ(vm.GetScrollY(), 130.0f);
}

TEST(ViewportManagerTest, AnchorCompensateNegativeIndex) {
    ViewportManager vm;
    auto cache = MakeTestCache(3, 100.0f);
    vm.SyncMaxScroll(300.0f, 100.0f);
    vm.ScrollTo(50.0f);
    float before = vm.GetScrollY();
    vm.AnchorCompensateScroll(-1, 0.0f, cache);
    EXPECT_FLOAT_EQ(vm.GetScrollY(), before); // 変更なし
}

// ---- 選択テスト ----

TEST(ViewportManagerTest, ClearSelection) {
    ViewportManager vm;
    vm.SetAnchor(3, 10);
    vm.SetDragging(true);
    vm.SetSelection(TextSelection::MakeOrdered(0, 0, 5, 20));

    vm.ClearSelection();
    EXPECT_FALSE(vm.GetSelection().active);
    EXPECT_EQ(vm.GetAnchorNode(), -1);
    EXPECT_FALSE(vm.IsDragging());
}

TEST(ViewportManagerTest, SelectAll) {
    std::pmr::vector<Node> nodes(3);
    nodes[0].text = L"hello";
    nodes[1].text = L"world";
    nodes[2].text = L"end";

    ViewportManager vm;
    vm.SelectAll(nodes);

    EXPECT_TRUE(vm.GetSelection().active);
    EXPECT_EQ(vm.GetSelection().start_node, 0);
    EXPECT_EQ(vm.GetSelection().start_pos, 0u);
    EXPECT_EQ(vm.GetSelection().end_node, 2);
    EXPECT_EQ(vm.GetSelection().end_pos, 3u);
}

TEST(ViewportManagerTest, SelectAllEmpty) {
    std::pmr::vector<Node> nodes;
    ViewportManager vm;
    vm.SelectAll(nodes);
    EXPECT_FALSE(vm.GetSelection().active);
}

// ---- ズームテスト ----

TEST(ViewportManagerTest, ZoomInReturnsNewZoom) {
    ViewportManager vm; // starts at ZOOM_DEFAULT_INDEX (1.0)
    float z = vm.ZoomIn();
    EXPECT_GT(z, 1.0f);
    EXPECT_EQ(vm.GetZoomIndex(), ZOOM_DEFAULT_INDEX + 1);
}

TEST(ViewportManagerTest, ZoomOutReturnsNewZoom) {
    ViewportManager vm;
    float z = vm.ZoomOut();
    EXPECT_LT(z, 1.0f);
    EXPECT_GT(z, 0.0f);
    EXPECT_EQ(vm.GetZoomIndex(), ZOOM_DEFAULT_INDEX - 1);
}

TEST(ViewportManagerTest, ZoomInAtMaxReturnsZero) {
    ViewportManager vm;
    // 最大までズーム
    for (int i = 0; i < 50; ++i) vm.ZoomIn();
    float z = vm.ZoomIn();
    EXPECT_FLOAT_EQ(z, 0.0f);
}

TEST(ViewportManagerTest, ZoomOutAtMinReturnsZero) {
    ViewportManager vm;
    // 最小までズーム
    for (int i = 0; i < 50; ++i) vm.ZoomOut();
    float z = vm.ZoomOut();
    EXPECT_FLOAT_EQ(z, 0.0f);
}

TEST(ViewportManagerTest, ZoomResetFromNonDefault) {
    ViewportManager vm;
    vm.ZoomIn();
    vm.ZoomIn();
    float z = vm.ZoomReset();
    EXPECT_FLOAT_EQ(z, 1.0f);
    EXPECT_EQ(vm.GetZoomIndex(), ZOOM_DEFAULT_INDEX);
}

TEST(ViewportManagerTest, ZoomResetAlreadyDefault) {
    ViewportManager vm;
    float z = vm.ZoomReset();
    EXPECT_FLOAT_EQ(z, 0.0f); // 変更不要
}

// ---- スクロールバー追跡テスト ----

TEST(ViewportManagerTest, ScrollbarTracking) {
    ViewportManager vm;
    EXPECT_FALSE(vm.IsScrollbarTracking());
    vm.SetScrollbarTracking(true);
    EXPECT_TRUE(vm.IsScrollbarTracking());
}

// ---- バグ #17: AnchorCompensateScrollの非負クランプ ----

TEST(ViewportManagerTest, AnchorCompensateScrollClampsNegative) {
    ViewportManager vm;
    auto cache = MakeTestCache(5, 100.0f);
    vm.SyncMaxScroll(500.0f, 200.0f);
    vm.ScrollTo(50.0f);

    float y_before = cache[2].y_position; // 200.0f
    // ノード2を上方に300シフトするレイアウト変更をシミュレーション
    // （例: 上にある大きなノードが削除された場合）
    cache[2].y_position = 0.0f;

    vm.AnchorCompensateScroll(2, y_before, cache);

    // scroll_yは-150ではなく0にクランプされるべき
    EXPECT_GE(vm.GetScrollY(), 0.0f);
    EXPECT_GE(vm.GetScrollTarget(), 0.0f);
}

TEST(ViewportManagerTest, AnchorCompensateScrollPositiveShiftOk) {
    ViewportManager vm;
    auto cache = MakeTestCache(5, 100.0f);
    vm.SyncMaxScroll(500.0f, 200.0f);
    vm.ScrollTo(100.0f);

    float y_before = cache[1].y_position; // 100.0f
    cache[1].y_position = 150.0f; // shifted down by 50

    vm.AnchorCompensateScroll(1, y_before, cache);
    EXPECT_FLOAT_EQ(vm.GetScrollY(), 150.0f);
}
