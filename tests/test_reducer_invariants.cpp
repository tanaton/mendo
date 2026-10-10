#include <gtest/gtest.h>
#include "document_test_helpers.h"
#include "reducer_harness.h"
#include "test_helpers.h"
#include <format>
#include <optional>
#include <random>
#include <string>
#include <vector>

// reducer の状態遷移列に対する不変条件テスト。
// 1 遷移の例示テスト (test_reducer.cpp) では、遷移の組み合わせでのみ起きる破れ
// (スクロール範囲外・マウスキャプチャ漏れ・スワイプ誤爆) を検出できないため、
// App 相当の前提を ReducerHarness で揃え、固定シードのランダム列で検査する。

namespace {

struct LabeledAction {
    std::string label;
    AppAction action;
};

std::vector<std::pmr::string> CollectHeadingAnchors(const ReducerHarness& h)
{
    std::vector<std::pmr::string> anchors;
    for (const auto& n : h.state.document.doc.GetNodes()) {
        if (n.type == NodeType::Heading && !n.anchor_id().empty()) {
            anchors.emplace_back(n.anchor_id());
        }
    }
    return anchors;
}

} // namespace

// ---- Home キー ----

// ロード直後 (scroll_y=0) と同じ位置に戻ること。ノード 0 の Top は上余白分だけ正なので、
// ノード 0 を target にした実装だと上余白が隠れた位置で止まる。
TEST(ReducerHomeKey, ReturnsToAbsoluteTopIncludingTopMargin)
{
    ReducerHarness h({ .markdown = MakeHarnessMarkdown(12) });
    ASSERT_GT(h.state.document.layout_cache.Top(0), 0.0f);
    ASSERT_GT(h.state.view.viewport.GetMaxScroll(), 300.0f);
    h.state.view.viewport.ScrollTo(300.0f);

    const auto effects = h.Dispatch(KeyScrollAction{ ScrollType::Home });

    EXPECT_FLOAT_EQ(h.state.view.viewport.GetScrollY(), 0.0f);
    EXPECT_TRUE(HasEffect<effect::InvalidateWindow>(effects));
}

// 1 画面に収まる文書 (max_scroll=0) で Home を押しても max_scroll を超えない。
TEST(ReducerHomeKey, ShortDocumentDoesNotExceedMaxScroll)
{
    ReducerHarness h({ .markdown = MakeHarnessMarkdown(1), .md_height = 2000.0f });
    ASSERT_FLOAT_EQ(h.state.view.viewport.GetMaxScroll(), 0.0f);

    h.Dispatch(KeyScrollAction{ ScrollType::Home });

    EXPECT_FLOAT_EQ(h.state.view.viewport.GetScrollY(), 0.0f);
}

// ---- スクロール不変条件 ----
// reducer 経由のどのスクロール操作の後も
//   (a) 0 <= scroll_y <= max_scroll
//   (b) scroll_y が変わったら 再描画 (InvalidateWindow) と目次同期 (SyncTocActive) を積む
// が成り立つこと。(a) が破れると後続のホイールで位置が一気に補正されて「飛ぶ」(issue#224)。

namespace {

struct ScrollLayoutCase {
    const char* name;
    int sections;
    float md_height;
    // 指定時は md ペイン高を総コンテンツ高からこの値だけ引いた高さに変え、max_scroll をこの値にする。
    std::optional<float> max_scroll;
};

void PrintTo(const ScrollLayoutCase& c, std::ostream* os)
{
    *os << c.name;
}

LabeledAction GenerateSingleScrollAction(std::mt19937& rng, const ReducerHarness& h, const std::vector<std::pmr::string>& anchors, uint64_t& tick)
{
    auto pick = [&](int lo, int hi) {
        return std::uniform_int_distribution<int>(lo, hi)(rng);
    };
    const int n = h.NodeCount();
    switch (pick(0, 12)) {
    case 0:
    case 1: {
        const auto t = static_cast<ScrollType>(pick(0, 5));
        return { std::format("KeyScroll({})", static_cast<int>(t)), KeyScrollAction{ t } };
    }
    case 2: {
        const float d = static_cast<float>(pick(-3000, 3000));
        return { std::format("DirectScrollBy({})", d), DirectScrollByAction{ d } };
    }
    case 3: {
        const float y = static_cast<float>(pick(-100, 2500));
        return { std::format("MdScrollbarDragStarted({})", y), MdScrollbarDragStartedAction{ y } };
    }
    case 4: {
        const float y = static_cast<float>(pick(-500, 3000));
        return { std::format("MdScrollbarDragMoved({})", y), MdScrollbarDragMovedAction{ y } };
    }
    case 5:
        return { "MdScrollbarDragEnded", MdScrollbarDragEndedAction{} };
    case 6: {
        const int node = pick(-1, n + 1);
        return { std::format("TocItemClicked({})", node), TocItemClickedAction{ node } };
    }
    case 7: {
        if (anchors.empty()) {
            return { "NoOp", NoOpAction{} };
        }
        const auto& a = anchors[static_cast<size_t>(pick(0, static_cast<int>(anchors.size()) - 1))];
        return { std::format("NavigateAnchor({})", a), NavigateAnchorAction{ a } };
    }
    case 8:
        return { "NavigateBack", NavigateBackAction{} };
    case 9:
        return { "NavigateForward", NavigateForwardAction{} };
    case 10: {
        switch (pick(0, 3)) {
        case 0:
            return { "SearchTextChanged(alpha)", SearchTextChangedAction{ std::pmr::wstring(L"alpha") } };
        case 1:
            return { "SearchNext", SearchNextAction{} };
        case 2:
            return { "SearchPrev", SearchPrevAction{} };
        default:
            return { "OpenSearchBar", OpenSearchBarAction{} };
        }
    }
    case 11: {
        // スワイプ (横ホイール蓄積 → コミットタイマー) 経由の戻る/進む。
        tick += 20;
        const short d = static_cast<short>(pick(0, 1) ? 480 : -480);
        return { std::format("HWheel({}, {})", d, tick), HWheelAction{ d, tick } };
    }
    default:
        return { "Timer(SWIPE_OVERLAY)", TimerAction{ app_timer::Id::SWIPE_OVERLAY } };
    }
}

std::vector<LabeledAction> GenerateScrollActions(std::mt19937& rng, const ReducerHarness& h, const std::vector<std::pmr::string>& anchors, uint64_t& tick)
{
    // 右ジェスチャは Started → 横移動 → Completed の一連でしか Back/Forward にならない。
    if (std::uniform_int_distribution<int>(0, 13)(rng) == 13) {
        const float dx = (rng() % 2) ? -200.0f : 200.0f;
        return {
            { "RightGestureStarted", RightClickGestureStartedAction{ 300.0f, 300.0f } },
            { std::format("RightGestureMoved(dx={})", dx), RightClickGestureMovedAction{ 300.0f + dx, 300.0f } },
            { "RightGestureCompleted", RightClickGestureCompletedAction{ 0, 0 } },
        };
    }
    return { GenerateSingleScrollAction(rng, h, anchors, tick) };
}

void RunScrollInvariant(const ScrollLayoutCase& lc, uint32_t seed)
{
    ReducerHarness h({ .markdown = MakeHarnessMarkdown(lc.sections), .md_height = lc.md_height });
    if (lc.max_scroll) {
        h.ResizeMdPane(MdScrollableContentHeight(h.state) - *lc.max_scroll);
    }
    const auto anchors = CollectHeadingAnchors(h);
    std::mt19937 rng(seed);
    uint64_t tick = 1000;
    std::string history;

    for (int step = 0; step < 250; ++step) {
        const auto seq = GenerateScrollActions(rng, h, anchors, tick);
        // App ではリサイズで md ペイン高と max_scroll が変わる。
        if (rng() % 16 == 0) {
            const float md_h = static_cast<float>(100 + rng() % 2000);
            h.ResizeMdPane(md_h);
            history += std::format(" Resize({})", md_h);
        }

        for (const auto& la : seq) {
            history += " " + la.label;
            SCOPED_TRACE(std::format("case={} seed={} step={} action={}", lc.name, seed, step, la.label));
            const float before = h.state.view.viewport.GetScrollY();
            const auto effects = h.Dispatch(la.action);
            const float y = h.state.view.viewport.GetScrollY();
            const float max_y = h.state.view.viewport.GetMaxScroll();
            ASSERT_GE(y, 0.0f) << "history:" << history;
            ASSERT_LE(y, max_y) << "history:" << history;
            if (y != before) {
                ASSERT_TRUE(HasEffect<effect::InvalidateWindow>(effects)) << "history:" << history;
                ASSERT_TRUE(HasEffect<effect::SyncTocActive>(effects)) << "history:" << history;
            }
        }
    }
}

} // namespace

class ReducerScrollInvariantTest : public ::testing::TestWithParam<ScrollLayoutCase> {};

TEST_P(ReducerScrollInvariantTest, ScrollStaysWithinRangeAndRedraws)
{
    for (uint32_t seed : { 1u, 7u, 42u, 1234u }) {
        RunScrollInvariant(GetParam(), seed);
        if (HasFatalFailure()) {
            return;
        }
    }
}

INSTANTIATE_TEST_SUITE_P(
    Layouts, ReducerScrollInvariantTest,
    ::testing::Values(
        ScrollLayoutCase{ "Scrollable", 12, 400.0f },
        ScrollLayoutCase{ "FitsInPane", 1, 2000.0f },
        // max_scroll が先頭ノードの Top より小さい: ノード基準の位置指定がそのまま範囲外になる。
        ScrollLayoutCase{ "TinyMaxScroll", 3, 400.0f, 5.0f }),
    [](const auto& info) { return std::string(info.param.name); });

// ---- マウスキャプチャ ----
// 不変条件: 「SetCapture 保持中」⇔「左ドラッグ進行中 (IsLeftDragActive) または右ジェスチャ進行中」。
// 保持したままドラッグ状態だけ消えると、LButtonUp がどの終了経路にも入らずキャプチャが残り、
// タイトルバーのドラッグやウィンドウ外への MouseLeave 検出が効かなくなる。
// 逆に、ドラッグ状態が残ったままキャプチャを失うと、ボタンを離しても終了処理が走らない。
//
// effects から OS 側のキャプチャ保持を追跡する。ReleaseCapture / 外部からの奪取では
// OS が同期的に WM_CAPTURECHANGED を送るため、CaptureChangedAction を入れ子で投入する。
// マウスイベントは App (app_mouse.cpp) と同じ振り分けでアクションに変換する。

namespace {

class CaptureModel {
public:
    explicit CaptureModel(ReducerHarness& h) : h_(h) {}

    void Dispatch(const AppAction& action)
    {
        Apply(h_.Dispatch(action));
    }

    // Alt+Tab や他ウィンドウの SetCapture によるキャプチャ喪失。
    void StealCapture()
    {
        if (held_) {
            held_ = false;
            Dispatch(CaptureChangedAction{});
        }
    }

    bool Held() const noexcept
    {
        return held_;
    }

private:
    void Apply(const SideEffectList& effects)
    {
        for (const auto& e : effects) {
            if (std::holds_alternative<effect::SetCapture>(e)) {
                held_ = true;
            }
            else if (std::holds_alternative<effect::ReleaseCapture>(e) && held_) {
                held_ = false;
                Dispatch(CaptureChangedAction{});
            }
        }
    }

    ReducerHarness& h_;
    bool held_ = false;
};

bool CaptureRequired(const AppState& s) noexcept
{
    return IsLeftDragActive(s) || s.interaction.gesture.GetPhase() != GesturePhase::Idle;
}

class CaptureDriver {
public:
    CaptureDriver(ReducerHarness& h, uint32_t seed) : h_(h), model_(h), rng_(seed) {}

    // 物理的に起こり得るイベントを 1 つ選んで実行し、ラベルを返す。
    std::string Step()
    {
        switch (Pick(0, 11)) {
        case 0:
        case 1:
            return left_down_ ? LButtonUp() : LButtonDown();
        case 2:
        case 3:
            return left_down_ ? LMove() : LButtonDown();
        case 4:
            return right_down_ ? RButtonUp() : RButtonDown();
        case 5:
            return RMove();
        case 6:
            return Key();
        case 7: {
            static constexpr app_timer::Id kTimers[] = {
                app_timer::Id::SWIPE_OVERLAY, app_timer::Id::TOAST, app_timer::Id::TOOLTIP,
                app_timer::Id::SEARCH_DEBOUNCE,
            };
            const auto id = kTimers[Pick(0, static_cast<int>(std::size(kTimers)) - 1)];
            model_.Dispatch(TimerAction{ id });
            return std::format("Timer({})", static_cast<int>(id));
        }
        case 8: {
            tick_ += 20;
            const short d = static_cast<short>(Pick(0, 1) ? 480 : -480);
            model_.Dispatch(HWheelAction{ d, tick_ });
            return std::format("HWheel({})", d);
        }
        case 9: {
            const int node = Pick(-1, h_.NodeCount());
            model_.Dispatch(BlockHHoverChangedAction{ node });
            return std::format("BlockHHover({})", node);
        }
        case 10:
            model_.StealCapture();
            return "StealCapture";
        default: {
            const bool active = Pick(0, 1) != 0;
            model_.Dispatch(ActivateAction{ active });
            model_.Dispatch(MouseLeaveAction{});
            return std::format("Activate({})+MouseLeave", active);
        }
        }
    }

    const CaptureModel& Model() const noexcept
    {
        return model_;
    }

private:
    int Pick(int lo, int hi)
    {
        return std::uniform_int_distribution<int>(lo, hi)(rng_);
    }

    float PickF(int lo, int hi)
    {
        return static_cast<float>(Pick(lo, hi));
    }

    // App::OnLButtonDown の各ゾーンからのドラッグ開始。
    std::string LButtonDown()
    {
        left_down_ = true;
        switch (Pick(0, 6)) {
        case 0:
        case 1: {
            const int node = Pick(-1, h_.NodeCount() - 1);
            model_.Dispatch(TextSelectionStartedAction{ node, static_cast<uint32_t>(Pick(0, 5)), 400, 100 });
            return std::format("LDown:TextSelection({})", node);
        }
        case 2: {
            const auto t = Pick(0, 1) ? PaneController::DragTarget::Splitter1 : PaneController::DragTarget::Splitter2;
            model_.Dispatch(SplitterDragStartedAction{ t });
            return "LDown:Splitter";
        }
        case 3:
            model_.Dispatch(MdScrollbarDragStartedAction{ PickF(0, 400) });
            return "LDown:MdScrollbar";
        case 4: {
            const auto pane = Pick(0, 1) ? PaneTarget::Toc : PaneTarget::File;
            model_.Dispatch(PaneScrollbarDragStartedAction{ pane, PickF(0, 120) });
            return "LDown:PaneScrollbar";
        }
        case 5: {
            const int node = Pick(-1, h_.NodeCount() - 1);
            model_.Dispatch(BlockHScrollDragStartedAction{ node, PickF(300, 900) });
            return std::format("LDown:BlockHScroll({})", node);
        }
        default:
            // 検索入力欄は検索バー表示中のみヒットする。
            if (h_.state.search.search_state.IsVisible()) {
                model_.Dispatch(SearchInputDragStartedAction{ Pick(0, 5) });
                return "LDown:SearchInput";
            }
            return "LDown:Nothing";
        }
    }

    // App::OnMouseMove の振り分け。
    std::string LMove()
    {
        const auto& s = h_.state;
        if (s.search.search_bar_ctrl.IsDragging()) {
            model_.Dispatch(SearchInputDragMovedAction{ Pick(0, 5) });
            return "LMove:SearchInput";
        }
        switch (const auto drag = s.view.panes.GetDragTarget()) {
        case PaneController::DragTarget::Splitter1:
        case PaneController::DragTarget::Splitter2:
            model_.Dispatch(SplitterDragMovedAction{ drag, PickF(50, 900), 1000.0f });
            return "LMove:Splitter";
        case PaneController::DragTarget::FileScrollbar:
            model_.Dispatch(PaneScrollbarDragMovedAction{ PaneTarget::File, PickF(0, 400) });
            return "LMove:FileScrollbar";
        case PaneController::DragTarget::TocScrollbar:
            model_.Dispatch(PaneScrollbarDragMovedAction{ PaneTarget::Toc, PickF(0, 400) });
            return "LMove:TocScrollbar";
        case PaneController::DragTarget::MdScrollbar:
            model_.Dispatch(MdScrollbarDragMovedAction{ PickF(0, 400) });
            return "LMove:MdScrollbar";
        case PaneController::DragTarget::None:
            break;
        }
        if (s.view.h_drag_node >= 0) {
            model_.Dispatch(BlockHScrollDragMovedAction{ PickF(300, 900) });
            return "LMove:BlockHScroll";
        }
        if (s.view.viewport.IsDragging()) {
            model_.Dispatch(TextSelectionMovedAction{ Pick(0, h_.NodeCount() - 1), static_cast<uint32_t>(Pick(0, 5)) });
            return "LMove:TextSelection";
        }
        return "LMove:Nothing";
    }

    // App::OnLButtonUp の振り分け。
    std::string LButtonUp()
    {
        left_down_ = false;
        const auto& s = h_.state;
        if (s.view.h_drag_node >= 0) {
            model_.Dispatch(BlockHScrollDragEndedAction{});
            return "LUp:BlockHScroll";
        }
        if (s.search.search_bar_ctrl.IsDragging()) {
            model_.Dispatch(SearchInputDragEndedAction{});
            return "LUp:SearchInput";
        }
        switch (s.view.panes.GetDragTarget()) {
        case PaneController::DragTarget::Splitter1:
        case PaneController::DragTarget::Splitter2:
            model_.Dispatch(SplitterDragEndedAction{});
            return "LUp:Splitter";
        case PaneController::DragTarget::MdScrollbar:
            model_.Dispatch(MdScrollbarDragEndedAction{});
            return "LUp:MdScrollbar";
        case PaneController::DragTarget::FileScrollbar:
        case PaneController::DragTarget::TocScrollbar:
            model_.Dispatch(PaneScrollbarDragEndedAction{});
            return "LUp:PaneScrollbar";
        case PaneController::DragTarget::None:
            break;
        }
        if (!s.view.viewport.IsDragging()) {
            return "LUp:Nothing";
        }
        model_.Dispatch(TextSelectionEndedAction{ Pick(-1, h_.NodeCount() - 1), 0 });
        return "LUp:TextSelection";
    }

    // App::OnRButtonDown: 左ドラッグ中はジェスチャを開始しない。
    std::string RButtonDown()
    {
        right_down_ = true;
        if (IsLeftDragActive(h_.state)) {
            return "RDown:Ignored";
        }
        model_.Dispatch(RightClickGestureStartedAction{ 300.0f, 300.0f });
        return "RDown:Gesture";
    }

    std::string RMove()
    {
        if (!right_down_) {
            return "RMove:Nothing";
        }
        const float x = PickF(50, 550);
        model_.Dispatch(RightClickGestureMovedAction{ x, 300.0f });
        return std::format("RMove({})", x);
    }

    std::string RButtonUp()
    {
        right_down_ = false;
        if (h_.state.interaction.gesture.GetPhase() == GesturePhase::Idle) {
            return "RUp:Nothing";
        }
        model_.Dispatch(RightClickGestureCompletedAction{ 0, 0 });
        return "RUp:Gesture";
    }

    std::string Key()
    {
        switch (Pick(0, 7)) {
        case 0:
        case 1:
            model_.Dispatch(ClearSelectionAction{});
            return "Key:Esc";
        case 2:
            model_.Dispatch(SelectAllAction{});
            return "Key:CtrlA";
        case 3:
            model_.Dispatch(OpenSearchBarAction{});
            return "Key:CtrlF";
        case 4:
            model_.Dispatch(KeyScrollAction{ static_cast<ScrollType>(Pick(0, 5)) });
            return "Key:Scroll";
        case 5:
            model_.Dispatch(NavigateBackAction{});
            return "Key:AltLeft";
        case 6:
            model_.Dispatch(SearchTextChangedAction{ std::pmr::wstring(L"alpha") });
            return "Key:SearchInput";
        default:
            model_.Dispatch(TocItemClickedAction{ Pick(0, h_.NodeCount() - 1) });
            return "Click:TocItem";
        }
    }

    ReducerHarness& h_;
    CaptureModel model_;
    std::mt19937 rng_;
    bool left_down_ = false;
    bool right_down_ = false;
    uint64_t tick_ = 1000;
};

} // namespace

TEST(ReducerCaptureInvariant, CaptureHeldIffDragOrGestureActive)
{
    for (uint32_t seed : { 1u, 2u, 3u, 99u, 2024u }) {
        ReducerHarness h({ .markdown = MakeHarnessMarkdown(12) });
        CaptureDriver driver(h, seed);
        std::string history;
        for (int step = 0; step < 400; ++step) {
            const std::string label = driver.Step();
            history += " " + label;
            SCOPED_TRACE(std::format("seed={} step={} event={}", seed, step, label));
            ASSERT_EQ(driver.Model().Held(), CaptureRequired(h.state))
                << "left_drag=" << IsLeftDragActive(h.state)
                << " gesture=" << static_cast<int>(h.state.interaction.gesture.GetPhase())
                << "\nhistory:" << history;
        }
    }
}

// テキスト選択ドラッグ中の ESC。ドラッグ状態ごと選択を解除するなら同時にキャプチャも返す。
TEST(ReducerCaptureInvariant, EscDuringTextDragReleasesCapture)
{
    ReducerHarness h({ .markdown = MakeHarnessMarkdown(3) });
    CaptureModel model(h);
    model.Dispatch(TextSelectionStartedAction{ 0, 0, 400, 100 });
    ASSERT_TRUE(model.Held());

    model.Dispatch(ClearSelectionAction{});

    EXPECT_FALSE(h.state.view.viewport.IsDragging());
    EXPECT_FALSE(model.Held());
}

// ---- 横ホイールとスワイプナビゲーション ----
// ブロック (テーブル/コードブロック) 上の横ホイールはブロックが吸収し、横スクロール
// できない場合でもスワイプ (戻る/進む) に流さない。過去 3 回 (b5e28d0 / 77d6cbd / 8e7c0db)
// 別経路で漏れて修正されたため、経路ごとに固定する。

namespace {

struct HWheelCase {
    const char* name;
    // ホバー/ドラッグ対象。kNone 以外は対象ノードの種別。
    int hover;
    int drag;
    bool code_fits;
    bool pane_layout_invalid;
    bool expect_swipe;
    bool expect_block_scroll;
};

constexpr int kNone = -1;
constexpr int kCodeBlock = -2;
constexpr int kParagraph = -3;
constexpr int kOutOfRange = -4;

void PrintTo(const HWheelCase& c, std::ostream* os)
{
    *os << c.name;
}

int ResolveNode(const ReducerHarness& h, int which)
{
    switch (which) {
    case kCodeBlock:
        return FindFirstNodeIndexByType(h.state.document.doc.GetNodes(), NodeType::CodeBlock);
    case kParagraph:
        return FindFirstNodeIndexByType(h.state.document.doc.GetNodes(), NodeType::Paragraph);
    case kOutOfRange:
        return h.NodeCount() + 5;
    default:
        return which;
    }
}

} // namespace

class ReducerHWheelSwipeTest : public ::testing::TestWithParam<HWheelCase> {};

TEST_P(ReducerHWheelSwipeTest, BlockAbsorbsHorizontalWheel)
{
    const auto& c = GetParam();
    ReducerHarness h({ .markdown = MakeHarnessMarkdown(4), .code_natural_width = c.code_fits ? 100.0f : 1500.0f });
    auto& s = h.state;
    s.view.nav_history.Push(NavEntry(L"C:\\docs\\b.md", 0, 0.0f));
    s.view.hovered_h_block = ResolveNode(h, c.hover);
    s.view.h_drag_node = ResolveNode(h, c.drag);
    const int target = s.view.h_drag_node >= 0 ? s.view.h_drag_node : s.view.hovered_h_block;
    const float block_x_before = s.view.GetBlockScrollX(target);

    // App::Dispatch は必ずペインレイアウトを確定させるため、無効ケースは Reduce を直接呼ぶ。
    auto dispatch = [&](const AppAction& a) {
        if (c.pane_layout_invalid) {
            s.pane_layout_cache.Invalidate();
            return Reduce(s, a);
        }
        return h.Dispatch(a);
    };

    bool timer_scheduled = false;
    bool overlay_shown = false;
    // 右スワイプ 4 ノッチ = 閾値 (400) 超え → 戻る。
    for (uint64_t i = 0; i < 4; ++i) {
        const auto effects = dispatch(HWheelAction{ 120, 1000 + i * 10 });
        const auto* t = FindEffect<effect::SetTimer>(effects);
        timer_scheduled |= (t && t->id == app_timer::Id::SWIPE_OVERLAY);
        overlay_shown |= s.interaction.swipe_detector.IsOverlayVisible();
    }
    const auto commit = dispatch(TimerAction{ app_timer::Id::SWIPE_OVERLAY });
    const bool navigated = HasEffect<effect::LoadFile>(commit);

    EXPECT_EQ(timer_scheduled, c.expect_swipe);
    EXPECT_EQ(overlay_shown, c.expect_swipe);
    EXPECT_EQ(navigated, c.expect_swipe);
    EXPECT_EQ(s.view.nav_history.CanGoBack(), !c.expect_swipe);
    EXPECT_EQ(s.view.GetBlockScrollX(target) > block_x_before, c.expect_block_scroll);
}

INSTANTIATE_TEST_SUITE_P(
    Cases, ReducerHWheelSwipeTest,
    ::testing::Values(
        // 対照: ブロック外なら従来どおりスワイプで戻る (テストの前提が成立していることの確認)。
        HWheelCase{ "NoBlock_Swipes", kNone, kNone, false, false, true, false },
        HWheelCase{ "HoverScrollableCode_Scrolls", kCodeBlock, kNone, false, false, false, true },
        HWheelCase{ "HoverCodeThatFits", kCodeBlock, kNone, true, false, false, false },
        HWheelCase{ "HoverParagraph", kParagraph, kNone, false, false, false, false },
        HWheelCase{ "HoverOutOfRangeNode", kOutOfRange, kNone, false, false, false, false },
        HWheelCase{ "HoverWithInvalidPaneLayout", kCodeBlock, kNone, false, true, false, false },
        HWheelCase{ "DraggingScrollableCode_Scrolls", kNone, kCodeBlock, false, false, false, true },
        HWheelCase{ "DraggingCodeThatFits", kNone, kCodeBlock, true, false, false, false },
        HWheelCase{ "DraggingWithInvalidPaneLayout", kNone, kCodeBlock, false, true, false, false }),
    [](const auto& info) { return std::string(info.param.name); });
