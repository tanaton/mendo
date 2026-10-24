#include <gtest/gtest.h>
#include "async_load_coordinator.h"
#include "task_scheduler.h"
#include "test_helpers.h"
#include "theme.h"
#include <atomic>
#include <condition_variable>
#include <future>
#include <random>
#include <vector>

namespace {

// scheduler は fixture で 1 度だけ Init/Shutdown する。各テストの Init/Shutdown は
// ワーカー thread の生成・join を毎回招きトータル数百ms に達するため。
class AsyncLoadCoordinatorTest : public ::testing::Test {
protected:
    static TaskScheduler scheduler_;

    static void SetUpTestSuite()
    {
        scheduler_.Init(1);
    }
    static void TearDownTestSuite()
    {
        scheduler_.Shutdown();
    }
};

TaskScheduler AsyncLoadCoordinatorTest::scheduler_;

// 結果が出るまで待って取り出す。時間切れなら nullopt。
std::optional<AsyncLoadOutcome> WaitForOutcome(AsyncLoadCoordinator& c)
{
    std::optional<AsyncLoadOutcome> outcome;
    PollUntil([&] {
        outcome = c.Take();
        return outcome.has_value();
    });
    return outcome;
}

} // namespace

TEST_F(AsyncLoadCoordinatorTest, DefaultConstructionIsInactive)
{
    AsyncLoadCoordinator c;
    EXPECT_FALSE(c.IsActive());
    EXPECT_FALSE(c.Take().has_value());
}

TEST_F(AsyncLoadCoordinatorTest, CancelOnIdleIsSafe)
{
    AsyncLoadCoordinator c;
    c.Cancel();
    EXPECT_FALSE(c.IsActive());
    EXPECT_FALSE(c.Take().has_value());
}

TEST_F(AsyncLoadCoordinatorTest, ShutdownSchedulerCausesReadFailedError)
{
    // Shutdown 済み scheduler への Post は false を返し、coordinator は同期的に
    // error_=ReadFailed を立てて in_flight_=false に戻す経路を踏む。
    TaskScheduler local;
    local.Init(1);
    local.Shutdown();

    AsyncLoadCoordinator c;
    c.Start(local, std::pmr::wstring(L"unused.md"), nullptr, 0, GetLightTheme());

    EXPECT_FALSE(c.IsActive());
    auto outcome = c.Take();
    ASSERT_TRUE(outcome.has_value());
    ASSERT_FALSE(outcome->has_value());
    EXPECT_EQ(outcome->error(), FileLoadError::ReadFailed);
    EXPECT_FALSE(c.Take().has_value());
}

// worker でメモリ確保に失敗しても terminate せず、大きすぎて読めなかった扱いで返す。
TEST_F(AsyncLoadCoordinatorTest, BadAllocInWorkerProducesTooLargeError)
{
    AsyncLoadCoordinator c{ [](const std::pmr::wstring&, const std::stop_token&) static -> std::expected<LoadedFileDoc, FileLoadError> {
        throw std::bad_alloc{};
    } };
    c.Start(scheduler_, std::pmr::wstring(L"unused.md"), nullptr, 0, GetLightTheme());

    auto outcome = WaitForOutcome(c);
    ASSERT_TRUE(outcome.has_value());
    ASSERT_FALSE(outcome->has_value());
    EXPECT_EQ(outcome->error(), FileLoadError::TooLarge);
}

TEST_F(AsyncLoadCoordinatorTest, SuccessfulLoadProducesResult)
{
    TempFile tmp(L"aload_ok", "# Title\n\nbody text\n");

    AsyncLoadCoordinator c;
    c.Start(scheduler_, tmp.PmrPath(), nullptr, 0, GetLightTheme());

    auto outcome = WaitForOutcome(c);
    ASSERT_TRUE(outcome.has_value());
    ASSERT_TRUE(outcome->has_value());
    const AsyncLoadResult& result = **outcome;
    EXPECT_EQ(result.cache.size(), result.doc.GetNodes().size());
    EXPECT_GT(result.cache.Top(0), 0.0f);
    EXPECT_FALSE(result.doc.IsEmpty());
    EXPECT_FALSE(c.IsActive());
}

TEST_F(AsyncLoadCoordinatorTest, NotFoundFileProducesError)
{
    AsyncLoadCoordinator c;
    c.Start(scheduler_, std::pmr::wstring(L"C:\\__mendo_no_such_file__.md"), nullptr, 0, GetLightTheme());

    auto outcome = WaitForOutcome(c);
    ASSERT_TRUE(outcome.has_value());
    ASSERT_FALSE(outcome->has_value());
    EXPECT_EQ(outcome->error(), FileLoadError::NotFound);
}

TEST_F(AsyncLoadCoordinatorTest, DestructorWaitsForRunningWorker)
{
    // dtor が走行中 worker の latch を待たないと共有 scheduler 経由で UAF。
    TempFile tmp(L"aload_dtor", "just text\n");
    {
        AsyncLoadCoordinator c;
        c.Start(scheduler_, tmp.PmrPath(), nullptr, 0, GetLightTheme());
    }
    SUCCEED();
}

// ---- リロード (reload_base 指定) ----

namespace {

std::optional<AsyncLoadResult> RunReload(TaskScheduler& scheduler, const TempFile& tmp, std::string_view old_text)
{
    auto base = std::make_shared<const std::pmr::string>(old_text);
    AsyncLoadCoordinator c;
    c.Start(scheduler, tmp.PmrPath(), nullptr, 0, GetLightTheme(), 0.0f, ReloadBase{ base });
    auto outcome = WaitForOutcome(c);
    std::optional<AsyncLoadResult> result;
    if (outcome && outcome->has_value()) {
        result = std::move(**outcome);
    }
    if (result && result->reload) {
        EXPECT_EQ(result->reload->base.lock(), base);
    }
    return result;
}

} // namespace

TEST_F(AsyncLoadCoordinatorTest, ReloadWithoutChangeSkipsParse)
{
    TempFile tmp(L"aload_reload_same", "# Title\n\nbody\n");
    auto result = RunReload(scheduler_, tmp, "# Title\n\nbody\n");
    ASSERT_TRUE(result.has_value());
    ASSERT_TRUE(result->reload.has_value());
    EXPECT_EQ(result->reload->decision.op, ReloadOp::NoChange);
    EXPECT_TRUE(result->doc.IsEmpty()) << "変更なしならパースしない";
}

// CRLF で保存し直されただけのファイルも、LF 正規化後の比較で変更なしになる (issue #273)。
TEST_F(AsyncLoadCoordinatorTest, ReloadNormalizesNewlinesBeforeDiff)
{
    TempFile tmp(L"aload_reload_crlf", "# Title\r\n\r\nbody\r\n");
    auto result = RunReload(scheduler_, tmp, "# Title\n\nbody\n");
    ASSERT_TRUE(result.has_value());
    ASSERT_TRUE(result->reload.has_value());
    EXPECT_EQ(result->reload->decision.op, ReloadOp::NoChange);
}

TEST_F(AsyncLoadCoordinatorTest, ReloadPrefixShrinkSkipsParse)
{
    TempFile tmp(L"aload_reload_shrink", "# Title\n");
    auto result = RunReload(scheduler_, tmp, "# Title\n\nbody\n");
    ASSERT_TRUE(result.has_value());
    ASSERT_TRUE(result->reload.has_value());
    EXPECT_EQ(result->reload->decision.op, ReloadOp::DeferPrefixShrink);
    EXPECT_TRUE(result->doc.IsEmpty());
}

TEST_F(AsyncLoadCoordinatorTest, ReloadWithChangeParsesAndCarriesDecision)
{
    TempFile tmp(L"aload_reload_change", "# Title\n\nchanged\n");
    auto result = RunReload(scheduler_, tmp, "# Title\n\nbody\n");
    ASSERT_TRUE(result.has_value());
    ASSERT_TRUE(result->reload.has_value());
    EXPECT_EQ(result->reload->decision.op, ReloadOp::Replace);
    EXPECT_EQ(result->reload->decision.diff_pos, 9u);
    EXPECT_FALSE(result->doc.IsEmpty());
    EXPECT_EQ(result->cache.size(), result->doc.GetNodes().size());
    EXPECT_GT(result->cache.Top(0), 0.0f);
    EXPECT_EQ(result->reload->loaded_byte_size, result->doc.GetLoadedByteSize());
}

// 旧文書のノードを渡すと、変わらなかったノードを worker で数えて返す。UI が旧文書を手放していれば数えない。
TEST_F(AsyncLoadCoordinatorTest, ReloadCountsUnchangedNodesOnWorker)
{
    TempFile tmp(L"aload_reload_ends", "# Title\n\nchanged\n\ntail\n");
    for (const bool keep_old : { true, false }) {
        SCOPED_TRACE(keep_old ? "keep_old" : "released");
        auto old_doc = std::make_optional(Document::FromMarkdown(std::pmr::string("# Title\n\nbody\n\ntail\n"), L"old.md"));
        ReloadBase base{ old_doc->GetRawText().Share(), old_doc->ShareNodes() };
        if (!keep_old) {
            base.nodes.reset();
        }
        AsyncLoadCoordinator c;
        c.Start(scheduler_, tmp.PmrPath(), nullptr, 0, GetLightTheme(), 0.0f, std::move(base));
        const auto outcome = WaitForOutcome(c);
        ASSERT_TRUE(outcome.has_value());
        ASSERT_TRUE(outcome->has_value());
        const auto& reload = (*outcome)->reload;
        ASSERT_TRUE(reload.has_value());
        ASSERT_EQ(reload->decision.op, ReloadOp::Replace);
        if (keep_old) {
            ASSERT_TRUE(reload->ends.has_value());
            EXPECT_EQ(reload->ends->head, 1u);
            EXPECT_EQ(reload->ends->tail, 1u);
        }
        else {
            EXPECT_FALSE(reload->ends.has_value());
        }
    }
}

// 読み終えた後もファイルが伸びている (書き込み途中) なら、差分判定もパースもせずに返す。
TEST_F(AsyncLoadCoordinatorTest, ReloadWhileFileStillGrowingSkipsParse)
{
    TempFile tmp(L"aload_reload_growing", std::string(256, 'x'));
    auto base = std::make_shared<const std::pmr::string>("# Title\n\nbody\n");
    // 実ファイルより短い先頭だけを読んだ状態を再現する。
    AsyncLoadCoordinator c([](const std::pmr::wstring&, const std::stop_token&) static -> std::expected<LoadedFileDoc, FileLoadError> {
        return LoadedFileDoc{ std::pmr::string("# Title\n\nchanged\n"), 17 };
    });
    c.Start(scheduler_, tmp.PmrPath(), nullptr, 0, GetLightTheme(), 0.0f, ReloadBase{ base });
    const auto outcome = WaitForOutcome(c);
    ASSERT_TRUE(outcome.has_value());
    ASSERT_TRUE(outcome->has_value());
    const auto& result = **outcome;
    ASSERT_TRUE(result.reload.has_value());
    EXPECT_EQ(result.reload->decision.op, ReloadOp::DeferPartialWrite);
    EXPECT_EQ(result.reload->loaded_byte_size, 17u);
    EXPECT_EQ(result.reload->base.lock(), base);
    EXPECT_TRUE(result.doc.IsEmpty());
}

// ---- 世代 / キャンセルの決定的検証 ----
// 単一 worker の scheduler を塞いで Start / Cancel を「worker 未着手」のまま積む、または
// 差し替えローダで worker を読み込み途中に止めることで、競合の順序をテスト側で固定する。

namespace {

constexpr UINT kParseCompleteMsg = WM_APP + 1;

// 単一 worker を占有し、以降に Post したタスクをキューに留める。
// 破棄時に必ず開くので、ASSERT で抜けても後続テストを道連れにしない。
class WorkerGate {
public:
    explicit WorkerGate(TaskScheduler& scheduler)
    {
        auto state = state_;
        const bool posted = scheduler.Post([state] {
            state->entered.store(true);
            state->entered.notify_all();
            state->open.wait(false);
        });
        EXPECT_TRUE(posted);
        if (posted) {
            state_->entered.wait(false);
        }
    }
    ~WorkerGate()
    {
        Open();
    }
    WorkerGate(const WorkerGate&) = delete;
    WorkerGate& operator=(const WorkerGate&) = delete;

    void Open() noexcept
    {
        state_->open.store(true);
        state_->open.notify_all();
    }

private:
    struct State {
        std::atomic<bool> entered{ false };
        std::atomic<bool> open{ false };
    };
    std::shared_ptr<State> state_ = std::make_shared<State>();
};

// 単一 worker は FIFO なので、番兵の完了はそれ以前に積んだタスク (PostMessage 含む) の完了を意味する。
[[nodiscard]] bool DrainScheduler(TaskScheduler& scheduler)
{
    auto done = std::make_shared<std::promise<void>>();
    auto finished = done->get_future();
    if (!scheduler.Post([done] { done->set_value(); })) {
        return false;
    }
    return finished.wait_for(std::chrono::seconds(5)) == std::future_status::ready;
}

// AsyncLoadCoordinator に差し込むローダ。1 回目の呼び出しだけ Open まで worker を止めて
// 指定結果を返し、2 回目以降は実ファイルを読む。関数ポインタで渡すため状態は static に置く。
struct LoaderGateState {
    std::mutex mutex;
    std::condition_variable cv;
    std::vector<std::stop_token> tokens;
    bool open = true;
    std::expected<LoadedFileDoc, FileLoadError> first_result = std::unexpected(FileLoadError::ReadFailed);
};

LoaderGateState& LoaderGate()
{
    static LoaderGateState state;
    return state;
}

std::expected<LoadedFileDoc, FileLoadError> GatedLoad(const std::pmr::wstring& path, const std::stop_token& stop_token)
{
    auto& g = LoaderGate();
    std::unique_lock lock(g.mutex);
    g.tokens.push_back(stop_token);
    g.cv.notify_all();
    if (g.tokens.size() > 1) {
        lock.unlock();
        return FileLoader::LoadFile(path);
    }
    g.cv.wait_for(lock, std::chrono::seconds(5), [&] { return g.open; });
    return g.first_result;
}

// coordinator より後に宣言すること。先に破棄されてゲートを開くので、coordinator の dtor が
// 止まった worker を待ってデッドロックしない。
class ArmedLoaderGate {
public:
    explicit ArmedLoaderGate(std::expected<LoadedFileDoc, FileLoadError> first_result)
    {
        auto& g = LoaderGate();
        const std::lock_guard lock(g.mutex);
        g.tokens.clear();
        g.open = false;
        g.first_result = std::move(first_result);
    }
    ~ArmedLoaderGate()
    {
        Open();
    }
    ArmedLoaderGate(const ArmedLoaderGate&) = delete;
    ArmedLoaderGate& operator=(const ArmedLoaderGate&) = delete;

    [[nodiscard]] bool WaitForCalls(size_t n)
    {
        auto& g = LoaderGate();
        std::unique_lock lock(g.mutex);
        return g.cv.wait_for(lock, std::chrono::seconds(5), [&] { return g.tokens.size() >= n; });
    }
    std::stop_token Token(size_t i)
    {
        auto& g = LoaderGate();
        const std::lock_guard lock(g.mutex);
        return i < g.tokens.size() ? g.tokens[i] : std::stop_token{};
    }
    size_t Calls()
    {
        auto& g = LoaderGate();
        const std::lock_guard lock(g.mutex);
        return g.tokens.size();
    }
    void Open()
    {
        auto& g = LoaderGate();
        {
            const std::lock_guard lock(g.mutex);
            g.open = true;
        }
        g.cv.notify_all();
    }
};

std::wstring_view ResultPath(const std::optional<AsyncLoadOutcome>& r)
{
    return r && r->has_value() ? std::wstring_view{ (*r)->doc.GetFilePath() } : std::wstring_view{};
}

} // namespace

TEST_F(AsyncLoadCoordinatorTest, RestartWhileQueuedPublishesOnlySecondLoad)
{
    TempFile tmp1(L"aload_first", "# first\n");
    TempFile tmp2(L"aload_second", "# second\n");
    MessageOnlyWindow w;
    AsyncLoadCoordinator c;
    {
        WorkerGate gate(scheduler_);
        c.Start(scheduler_, tmp1.PmrPath(), w.Get(), kParseCompleteMsg, GetLightTheme());
        c.Start(scheduler_, tmp2.PmrPath(), w.Get(), kParseCompleteMsg, GetLightTheme());
        EXPECT_TRUE(c.IsActive());
    }
    ASSERT_TRUE(DrainScheduler(scheduler_));

    // 旧世代の worker が publish すると通知が 2 通になり、App は OnParseComplete を 2 回走らせる。
    EXPECT_EQ(w.DrainMessages(kParseCompleteMsg), 1);
    auto outcome = c.Take();
    ASSERT_TRUE(outcome.has_value());
    ASSERT_TRUE(outcome->has_value());
    EXPECT_EQ(ResultPath(outcome), tmp2.PmrPath());
    EXPECT_FALSE(c.Take().has_value());
    EXPECT_FALSE(c.IsActive());
}

TEST_F(AsyncLoadCoordinatorTest, CancelWhileQueuedPublishesNothing)
{
    TempFile tmp(L"aload_cancel", "# heading\n\nparagraph\n");
    MessageOnlyWindow w;
    AsyncLoadCoordinator c;
    {
        WorkerGate gate(scheduler_);
        c.Start(scheduler_, tmp.PmrPath(), w.Get(), kParseCompleteMsg, GetLightTheme());
        c.Cancel();
        EXPECT_FALSE(c.IsActive());
    }
    ASSERT_TRUE(DrainScheduler(scheduler_));

    EXPECT_EQ(w.DrainMessages(kParseCompleteMsg), 0);
    EXPECT_FALSE(c.Take().has_value());
}

// worker が入口の世代チェックを通過した後 (読み込み中) に Cancel された場合、エラーの publish は
// lock 内の世代再確認でしか弾けない。
TEST_F(AsyncLoadCoordinatorTest, CancelDuringLoadDiscardsLateError)
{
    MessageOnlyWindow w;
    AsyncLoadCoordinator c(&GatedLoad);
    ArmedLoaderGate gate{ std::unexpected(FileLoadError::NotFound) };

    c.Start(scheduler_, std::pmr::wstring(L"gated.md"), w.Get(), kParseCompleteMsg, GetLightTheme());
    ASSERT_TRUE(gate.WaitForCalls(1));
    EXPECT_FALSE(gate.Token(0).stop_requested());
    c.Cancel();
    EXPECT_TRUE(gate.Token(0).stop_requested()) << "Cancel は走行中 worker の stop_token を要求する";
    gate.Open();
    ASSERT_TRUE(DrainScheduler(scheduler_));

    EXPECT_EQ(w.DrainMessages(kParseCompleteMsg), 0);
    EXPECT_FALSE(c.Take().has_value());
    EXPECT_FALSE(c.IsActive());
}

TEST_F(AsyncLoadCoordinatorTest, RestartDuringLoadStopsPreviousWorkerAndDiscardsItsError)
{
    TempFile tmp2(L"aload_restart_second", "# second\n");
    MessageOnlyWindow w;
    AsyncLoadCoordinator c(&GatedLoad);
    ArmedLoaderGate gate{ std::unexpected(FileLoadError::ReadFailed) };

    c.Start(scheduler_, std::pmr::wstring(L"gated.md"), w.Get(), kParseCompleteMsg, GetLightTheme());
    ASSERT_TRUE(gate.WaitForCalls(1));
    c.Start(scheduler_, tmp2.PmrPath(), w.Get(), kParseCompleteMsg, GetLightTheme());
    // 前 worker の parse / Estimate を協調キャンセルできるよう、キャプチャ済み token に停止が届く。
    EXPECT_TRUE(gate.Token(0).stop_requested());
    gate.Open();
    ASSERT_TRUE(DrainScheduler(scheduler_));

    ASSERT_EQ(gate.Calls(), 2u);
    EXPECT_FALSE(gate.Token(1).stop_requested()) << "新しい worker の token は作り直した source 由来";
    EXPECT_EQ(w.DrainMessages(kParseCompleteMsg), 1);
    auto outcome = c.Take();
    ASSERT_TRUE(outcome.has_value());
    ASSERT_TRUE(outcome->has_value()) << "旧世代のエラーが漏れると App は失敗トーストを出す";
    EXPECT_EQ(ResultPath(outcome), tmp2.PmrPath());
}

// Start / Cancel のランダム列を worker 未着手のまま積み、最後の操作だけが観測されることを確かめる。
TEST_F(AsyncLoadCoordinatorTest, RandomStartCancelSequencesExposeOnlyLastLoad)
{
    TempFile ok1(L"aload_rand_one", "# one\n");
    TempFile ok2(L"aload_rand_two", "# two\n\nbody\n");
    const std::pmr::wstring missing(L"C:\\__mendo_no_such_file_rand__.md");
    const auto ok1_base = std::make_shared<const std::pmr::string>("# one\n");

    enum class Expect : uint8_t { None, Ok1, Ok2, Missing, Ok1NoChange };
    constexpr std::string_view kOpNames[] = { "Start(ok1)", "Start(ok2)", "Start(missing)", "Start(ok1,reload)", "Cancel" };

    MessageOnlyWindow w;
    for (const uint32_t seed : { 1u, 7u, 42u, 2024u }) {
        std::mt19937 rng(seed);
        AsyncLoadCoordinator c;
        for (int burst = 0; burst < 25; ++burst) {
            std::string ops;
            Expect expect = Expect::None;
            {
                WorkerGate gate(scheduler_);
                const int n = std::uniform_int_distribution<int>(1, 4)(rng);
                for (int i = 0; i < n; ++i) {
                    const int op = std::uniform_int_distribution<int>(0, 4)(rng);
                    ops += kOpNames[op];
                    ops += ' ';
                    switch (op) {
                    case 0:
                        c.Start(scheduler_, ok1.PmrPath(), w.Get(), kParseCompleteMsg, GetLightTheme());
                        expect = Expect::Ok1;
                        break;
                    case 1:
                        c.Start(scheduler_, ok2.PmrPath(), w.Get(), kParseCompleteMsg, GetLightTheme());
                        expect = Expect::Ok2;
                        break;
                    case 2:
                        c.Start(scheduler_, missing, w.Get(), kParseCompleteMsg, GetLightTheme());
                        expect = Expect::Missing;
                        break;
                    case 3:
                        c.Start(scheduler_, ok1.PmrPath(), w.Get(), kParseCompleteMsg, GetLightTheme(), 0.0f, ReloadBase{ ok1_base });
                        expect = Expect::Ok1NoChange;
                        break;
                    default:
                        c.Cancel();
                        expect = Expect::None;
                        break;
                    }
                    EXPECT_EQ(c.IsActive(), expect != Expect::None) << ops;
                }
                // worker 未着手なので、どの世代の結果もまだ見えてはいけない。
                EXPECT_FALSE(c.Take().has_value()) << ops;
            }
            SCOPED_TRACE(::testing::Message() << "seed=" << seed << " burst=" << burst << " ops=" << ops);
            ASSERT_TRUE(DrainScheduler(scheduler_));

            EXPECT_EQ(w.DrainMessages(kParseCompleteMsg), expect == Expect::None ? 0 : 1);
            auto outcome = c.Take();
            switch (expect) {
            case Expect::None:
                EXPECT_FALSE(outcome.has_value());
                break;
            case Expect::Ok1:
            case Expect::Ok2:
                ASSERT_TRUE(outcome.has_value());
                ASSERT_TRUE(outcome->has_value());
                EXPECT_FALSE((*outcome)->reload.has_value());
                EXPECT_EQ(ResultPath(outcome), (expect == Expect::Ok1 ? ok1 : ok2).PmrPath());
                break;
            case Expect::Missing:
                ASSERT_TRUE(outcome.has_value());
                ASSERT_FALSE(outcome->has_value());
                EXPECT_EQ(outcome->error(), FileLoadError::NotFound);
                break;
            case Expect::Ok1NoChange:
                ASSERT_TRUE(outcome.has_value());
                ASSERT_TRUE(outcome->has_value());
                ASSERT_TRUE((*outcome)->reload.has_value());
                EXPECT_EQ((*outcome)->reload->decision.op, ReloadOp::NoChange);
                EXPECT_EQ((*outcome)->reload->path, ok1.PmrPath());
                break;
            }
            EXPECT_FALSE(c.IsActive());
        }
    }
}

// 取り消し済みのロードが worker で bad_alloc になっても、結果を出さない。
// 世代の確認を通らずに書くと、取り消したファイルが「大きすぎます」として表示される。
TEST_F(AsyncLoadCoordinatorTest, BadAllocAfterCancelProducesNoOutcome)
{
    // ローダーは関数ポインタなので、解放の合図は static で渡す。
    static std::shared_future<void> released;
    std::promise<void> release;
    released = release.get_future().share();
    AsyncLoadCoordinator c{ [](const std::pmr::wstring&, const std::stop_token&) static -> std::expected<LoadedFileDoc, FileLoadError> {
        released.wait();
        throw std::bad_alloc{};
    } };
    c.Start(scheduler_, std::pmr::wstring(L"unused.md"), nullptr, 0, GetLightTheme());
    c.Cancel();
    release.set_value();

    ASSERT_TRUE(DrainScheduler(scheduler_));

    EXPECT_FALSE(c.Take().has_value());
    EXPECT_FALSE(c.IsActive());
}

// ---- 起動時の読み込み (通知先を後から渡す) ----

TEST_F(AsyncLoadCoordinatorTest, AttachReturnsNoneWhenNotStarted)
{
    AsyncLoadCoordinator c;
    MessageOnlyWindow w;
    EXPECT_EQ(c.Attach(w.Get(), WM_USER + 1), AsyncLoadCoordinator::AttachResult::None);
}

// 通知先なしで始めて結果が先に出たら、Attach は通知せずにその場で取り込ませる。
TEST_F(AsyncLoadCoordinatorTest, AttachAfterOutcomeIsAppliedSync)
{
    TempFile tmp(L"coord_preload_sync", "# attach after done\n");
    MessageOnlyWindow w;
    AsyncLoadCoordinator c;
    c.Start(scheduler_, tmp.PmrPath(), nullptr, 0, GetLightTheme());
    ASSERT_TRUE(DrainScheduler(scheduler_));

    EXPECT_EQ(c.Attach(w.Get(), WM_USER + 1), AsyncLoadCoordinator::AttachResult::AppliedSync);
    auto outcome = c.Take();
    ASSERT_TRUE(outcome.has_value());
    ASSERT_TRUE(outcome->has_value());
    const AsyncLoadResult& result = **outcome;
    // cache は worker で推定済みの状態で渡る。
    ASSERT_EQ(result.cache.size(), result.doc.GetNodes().size());
    EXPECT_GT(result.cache[0].height, 0.0f);
    EXPECT_EQ(w.DrainMessages(WM_USER + 1), 0);
    EXPECT_FALSE(c.IsActive());
}

// 結果より先に Attach したら、結果を出したときに渡した通知先へ送る。
TEST_F(AsyncLoadCoordinatorTest, AttachBeforeOutcomeNotifiesLater)
{
    static std::shared_future<void> released;
    std::promise<void> release;
    released = release.get_future().share();
    AsyncLoadCoordinator c{ [](const std::pmr::wstring&, const std::stop_token&) static -> std::expected<LoadedFileDoc, FileLoadError> {
        released.wait();
        return LoadedFileDoc{ .text = std::pmr::string{ "# late\n" }, .byte_size = 7 };
    } };
    MessageOnlyWindow w;
    c.Start(scheduler_, std::pmr::wstring(L"unused.md"), nullptr, 0, GetLightTheme());
    EXPECT_EQ(c.Attach(w.Get(), WM_USER + 1), AsyncLoadCoordinator::AttachResult::AttachedAsync);
    release.set_value();
    ASSERT_TRUE(DrainScheduler(scheduler_));

    EXPECT_EQ(w.DrainMessages(WM_USER + 1), 1);
    auto outcome = c.Take();
    ASSERT_TRUE(outcome.has_value());
    EXPECT_TRUE(outcome->has_value());
}

// 推定は渡したテーマ (ズーム適用済み) の寸法で行う。
TEST_F(AsyncLoadCoordinatorTest, EstimatesWithGivenThemeZoom)
{
    TempFile tmp(L"coord_preload_zoom", "# heading\n\nbody\n");
    const auto estimated_top = [&](float zoom) {
        Theme theme = GetLightTheme();
        theme.ApplyZoom(zoom);
        AsyncLoadCoordinator c;
        c.Start(scheduler_, tmp.PmrPath(), nullptr, 0, theme);
        EXPECT_TRUE(DrainScheduler(scheduler_));
        auto outcome = c.Take();
        const bool ok = outcome && outcome->has_value();
        EXPECT_TRUE(ok);
        return ok ? (*outcome)->cache.Top(1) : 0.0f;
    };
    EXPECT_FLOAT_EQ(estimated_top(2.0f), estimated_top(1.0f) * 2.0f);
}
