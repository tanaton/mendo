#include <gtest/gtest.h>
#include "viewport_manager.h"
#include "pane_layout.h"
#include "test_helpers.h"
#include "document_types.h"
#include <tuple>
#include <vector>

// ---- スクロールテスト ----

// ScrollTo は絶対位置、DirectScrollBy は相対量で動かし、どちらも [0, max] に収める。
TEST(ViewportManagerTest, ScrollClampsToRange)
{
    struct Case {
        const char* name;
        bool direct;
        float content_h;
        float viewport_h;
        std::vector<float> amounts;
        float expected;
    };
    const Case kCases[] = {
        { "ScrollToBelowZero", false, 1000.0f, 500.0f, { 0.0f, -100.0f }, 0.0f },
        { "ScrollToAboveMax", false, 1000.0f, 500.0f, { 0.0f, 9999.0f }, 500.0f },
        { "ScrollToShortContent", false, 100.0f, 500.0f, { 50.0f }, 0.0f },
        { "DirectScrollByMoves", true, 10000.0f, 200.0f, { 0.0f, 100.0f }, 100.0f },
        { "DirectScrollByAboveMax", true, 300.0f, 200.0f, { 0.0f, 9999.0f }, 100.0f },
        { "DirectScrollByBelowZero", true, 1000.0f, 200.0f, { 50.0f, -200.0f }, 0.0f },
        // タッチパッドの連続入力
        { "DirectScrollByAccumulates", true, 100000.0f, 200.0f, std::vector<float>(30, 24.0f), 720.0f },
    };
    for (const auto& c : kCases) {
        SCOPED_TRACE(c.name);
        ViewportManager vm;
        const float max_scroll = MaxScroll(c.content_h, c.viewport_h);
        for (const float amount : c.amounts) {
            if (c.direct) {
                vm.DirectScrollBy(amount, max_scroll);
            }
            else {
                vm.ScrollTo(amount, max_scroll);
            }
        }
        EXPECT_FLOAT_EQ(vm.GetScrollY(), c.expected);
    }
    EXPECT_FLOAT_EQ(MaxScroll(100.0f, 500.0f), 0.0f);
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

// 利用者のスクロールは、ノード基準の位置を捨てる。
TEST(ViewportManagerTest, UserScrollInvalidatesScrollTarget)
{
    ViewportManager by_to;
    by_to.SetScrollTarget(2, 10.0f);
    EXPECT_TRUE(by_to.GetScrollTarget().IsValid());
    by_to.ScrollTo(100.0f, 0.0f);
    EXPECT_FALSE(by_to.GetScrollTarget().IsValid());

    ViewportManager by_delta;
    by_delta.SetScrollTarget(2, 10.0f);
    by_delta.DirectScrollBy(50.0f, MaxScroll(1000.0f, 200.0f));
    EXPECT_FALSE(by_delta.GetScrollTarget().IsValid());
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

// 全選択は先頭ノードの 0 から最終ノードの末尾まで。最終ノードが Table のときも concat_text (= GetText) の末尾を終端にする。
TEST(ViewportManagerTest, SelectAllAndClear)
{
    std::vector<Node> nodes(3);
    nodes[0].SetText("hello");
    nodes[1].SetText("world");
    nodes[2].SetText("end");
    std::vector<Node> table_last;
    table_last.push_back(MakeTextNode("hello"));
    table_last.push_back(MakeTableNode({ { "cell0", "cell1" } }));
    const auto table_end = static_cast<uint32_t>(table_last[1].table_data()->concat_text.size());

    for (const auto& [doc, end_node, end_pos] : { std::tuple{ &nodes, 2, 3u }, std::tuple{ &table_last, 1, table_end } }) {
        SCOPED_TRACE(end_node);
        ViewportManager vm;
        vm.SelectAll(*doc);
        ASSERT_TRUE(vm.GetSelection().active());
        EXPECT_EQ(vm.GetSelection().start_node, 0);
        EXPECT_EQ(vm.GetSelection().start_pos, 0u);
        EXPECT_EQ(vm.GetSelection().end_node, end_node);
        EXPECT_EQ(vm.GetSelection().end_pos, end_pos);

        vm.ClearSelection();
        EXPECT_FALSE(vm.GetSelection().active());
    }

    ViewportManager empty;
    empty.SelectAll({});
    EXPECT_FALSE(empty.GetSelection().active());
}

// ---- ズームテスト ----

// 段は既定 (1.0) から始まり、端では動かず false を返す。
TEST(ViewportManagerTest, ZoomStep)
{
    const ViewportManager fresh;
    EXPECT_EQ(fresh.Zoom().Index(), ZOOM_DEFAULT_INDEX);
    struct Case {
        int start;
        ZoomDirection direction;
        int expected;
        bool changed;
    };
    constexpr Case kCases[] = {
        { ZOOM_DEFAULT_INDEX, ZoomDirection::In, ZOOM_DEFAULT_INDEX + 1, true },
        { ZOOM_DEFAULT_INDEX, ZoomDirection::Out, ZOOM_DEFAULT_INDEX - 1, true },
        { ZOOM_STEP_COUNT - 1, ZoomDirection::In, ZOOM_STEP_COUNT - 1, false },
        { 0, ZoomDirection::Out, 0, false },
        { ZOOM_DEFAULT_INDEX + 2, ZoomDirection::Reset, ZOOM_DEFAULT_INDEX, true },
        { ZOOM_DEFAULT_INDEX, ZoomDirection::Reset, ZOOM_DEFAULT_INDEX, false },
    };
    for (const auto& c : kCases) {
        SCOPED_TRACE(::testing::Message() << c.start << " dir=" << static_cast<int>(c.direction));
        ViewportManager vm;
        vm.Zoom().SetIndex(c.start);
        EXPECT_EQ(vm.Zoom().Step(c.direction), c.changed);
        EXPECT_EQ(vm.Zoom().Index(), c.expected);
        EXPECT_FLOAT_EQ(vm.Zoom().Value(), ZOOM_STEPS[c.expected]);
    }
}

TEST(ViewportManagerTest, SetZoomIndexClampsToRange)
{
    const std::pair<int, int> cases[] = { { -5, 0 }, { ZOOM_STEP_COUNT + 100, ZOOM_STEP_COUNT - 1 }, { 3, 3 } };
    for (const auto [index, expected] : cases) {
        SCOPED_TRACE(index);
        ViewportManager vm;
        vm.Zoom().SetIndex(index);
        EXPECT_EQ(vm.Zoom().Index(), expected);
        EXPECT_FLOAT_EQ(vm.Zoom().Value(), ZOOM_STEPS[expected]);
    }
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
