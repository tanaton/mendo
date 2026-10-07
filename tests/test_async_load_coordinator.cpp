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

} // namespace

TEST_F(AsyncLoadCoordinatorTest, DefaultConstructionIsInactive)
{
    AsyncLoadCoordinator c;
    EXPECT_FALSE(c.IsActive());
    EXPECT_FALSE(c.TakeResult().has_value());
    EXPECT_FALSE(c.TakeError().has_value());
}

TEST_F(AsyncLoadCoordinatorTest, CancelOnIdleIsSafe)
{
    AsyncLoadCoordinator c;
    c.Cancel();
    EXPECT_FALSE(c.IsActive());
    EXPECT_FALSE(c.TakeResult().has_value());
    EXPECT_FALSE(c.TakeError().has_value());
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
    auto err = c.TakeError();
    ASSERT_TRUE(err.has_value());
    EXPECT_EQ(*err, FileLoadError::ReadFailed);
    EXPECT_FALSE(c.TakeError().has_value());
}

TEST_F(AsyncLoadCoordinatorTest, SuccessfulLoadProducesResult)
{
    TempFile tmp(L"aload_ok", "# Title\n\nbody text\n");

    AsyncLoadCoordinator c;
    c.Start(scheduler_, tmp.PmrPath(), nullptr, 0, GetLightTheme());

    std::optional<AsyncLoadResult> result;
    PollUntil([&] { result = c.TakeResult(); return result.has_value(); });
    ASSERT_TRUE(result.has_value());
    EXPECT_TRUE(result->heights_estimated);
    EXPECT_FALSE(result->doc.IsEmpty());
    EXPECT_FALSE(c.IsActive());
}

TEST_F(AsyncLoadCoordinatorTest, NotFoundFileProducesError)
{
    AsyncLoadCoordinator c;
    c.Start(scheduler_, std::pmr::wstring(L"C:\\__mendo_no_such_file__.md"), nullptr, 0, GetLightTheme());

    std::optional<FileLoadError> err;
    PollUntil([&] { err = c.TakeError(); return err.has_value(); });
    ASSERT_TRUE(err.has_value());
    EXPECT_EQ(*err, FileLoadError::NotFound);
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
    c.Start(scheduler, tmp.PmrPath(), nullptr, 0, GetLightTheme(), base);
    std::optional<AsyncLoadResult> result;
    PollUntil([&] { result = c.TakeResult(); return result.has_value(); });
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
    EXPECT_EQ(result->reload->decision.op, ReloadOp::FullReload);
    EXPECT_EQ(result->reload->decision.diff_pos, 9u);
    EXPECT_FALSE(result->doc.IsEmpty());
    EXPECT_TRUE(result->heights_estimated);
    EXPECT_EQ(result->reload->loaded_byte_size, result->doc.GetLoadedByteSize());
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

std::wstring_view ResultPath(const std::optional<AsyncLoadResult>& r)
{
    return r ? std::wstring_view{ r->doc.GetFilePath() } : std::wstring_view{};
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
    auto result = c.TakeResult();
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(ResultPath(result), tmp2.PmrPath());
    EXPECT_FALSE(c.TakeResult().has_value());
    EXPECT_FALSE(c.TakeError().has_value());
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
    EXPECT_FALSE(c.TakeResult().has_value());
    EXPECT_FALSE(c.TakeError().has_value());
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
    EXPECT_FALSE(c.TakeError().has_value());
    EXPECT_FALSE(c.TakeResult().has_value());
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
    EXPECT_FALSE(c.TakeError().has_value()) << "旧世代のエラーが漏れると App は失敗トーストを出す";
    auto result = c.TakeResult();
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(ResultPath(result), tmp2.PmrPath());
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
                        c.Start(scheduler_, ok1.PmrPath(), w.Get(), kParseCompleteMsg, GetLightTheme(), ok1_base);
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
                EXPECT_FALSE(c.TakeResult().has_value()) << ops;
                EXPECT_FALSE(c.TakeError().has_value()) << ops;
            }
            SCOPED_TRACE(::testing::Message() << "seed=" << seed << " burst=" << burst << " ops=" << ops);
            ASSERT_TRUE(DrainScheduler(scheduler_));

            EXPECT_EQ(w.DrainMessages(kParseCompleteMsg), expect == Expect::None ? 0 : 1);
            auto result = c.TakeResult();
            auto error = c.TakeError();
            switch (expect) {
            case Expect::None:
                EXPECT_FALSE(result.has_value());
                EXPECT_FALSE(error.has_value());
                break;
            case Expect::Ok1:
            case Expect::Ok2:
                ASSERT_TRUE(result.has_value());
                EXPECT_FALSE(error.has_value());
                EXPECT_FALSE(result->reload.has_value());
                EXPECT_EQ(ResultPath(result), (expect == Expect::Ok1 ? ok1 : ok2).PmrPath());
                break;
            case Expect::Missing:
                EXPECT_FALSE(result.has_value());
                ASSERT_TRUE(error.has_value());
                EXPECT_EQ(*error, FileLoadError::NotFound);
                break;
            case Expect::Ok1NoChange:
                ASSERT_TRUE(result.has_value());
                EXPECT_FALSE(error.has_value());
                ASSERT_TRUE(result->reload.has_value());
                EXPECT_EQ(result->reload->decision.op, ReloadOp::NoChange);
                EXPECT_EQ(result->reload->path, ok1.PmrPath());
                break;
            }
            EXPECT_FALSE(c.IsActive());
        }
    }
}
