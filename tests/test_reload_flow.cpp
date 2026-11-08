#include <gtest/gtest.h>
#include "async_load_coordinator.h"
#include "app_constants.h"
#include <iterator>
#include <memory>
#include <optional>
#include <ostream>
#include <random>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

// App::OnParseComplete の分岐 (PlanParseComplete) を表形式で網羅する。
// 7bf29e2 / b5e28d0 / 8e7c0db で修正が繰り返された箇所: stale 通知、判定後の文書差し替え、
// 書き込み途中の延期、差分判定の各結果。

namespace {

constexpr std::wstring_view kPath = L"C:\\docs\\note.md";
constexpr std::wstring_view kPathOtherCase = L"c:\\DOCS\\NOTE.MD";
constexpr std::wstring_view kOtherPath = L"C:\\docs\\other.md";
constexpr std::string_view kText = "# Title\n\nbody\n";

enum class ResultKind : uint8_t {
    None,
    // 表示中でないファイルを読んだ結果 (reload_base なしで Start された)。
    Doc,
    // reload_base 付きで Start され、worker が差分判定を済ませた結果。
    Reload,
};

enum class BaseRef : uint8_t {
    Current, // 表示中の文書そのもの
    Other,   // 別の文書 (判定後に表示文書が差し替わった)
    Expired, // 旧文書は既に解放済み
};

struct Case {
    const char* label;
    // 表示中の文書
    std::wstring_view current_path = kPath;
    std::string_view current_text = kText;
    // AsyncLoadCoordinator から取り出した値
    ResultKind kind = ResultKind::None;
    std::optional<FileLoadError> error;
    // Doc
    std::wstring_view result_path = kPath;
    std::string_view result_text = kText;
    // Reload
    ReloadOp reload_op = ReloadOp::NoChange;
    size_t reload_diff_pos = std::string_view::npos;
    BaseRef base = BaseRef::Current;

    // 期待値
    ParseCompleteStep step;
};

void PrintTo(const Case& c, std::ostream* os)
{
    *os << c.label;
}

struct Fixture {
    Document current;
    std::shared_ptr<const DocumentData> other_base;
    std::optional<AsyncLoadOutcome> outcome;
};

Fixture Build(const Case& c)
{
    Fixture f;
    if (!c.current_path.empty() || !c.current_text.empty()) {
        f.current = Document::FromMarkdown(std::string(c.current_text), c.current_path);
    }
    switch (c.kind) {
    case ResultKind::None:
        if (c.error) {
            f.outcome.emplace(std::unexpected(*c.error));
        }
        break;
    case ResultKind::Doc:
        f.outcome.emplace(AsyncLoadResult{ Document::FromMarkdown(std::string(c.result_text), c.result_path) });
        break;
    case ResultKind::Reload: {
        std::weak_ptr<const DocumentData> base;
        switch (c.base) {
        case BaseRef::Current:
            base = f.current.ShareData();
            break;
        case BaseRef::Other:
            f.other_base = Document::FromMarkdown(std::string(c.current_text), c.current_path).ShareData();
            base = f.other_base;
            break;
        case BaseRef::Expired:
            base = Document::FromMarkdown(std::string(c.current_text), c.current_path).ShareData();
            break;
        }
        f.outcome.emplace(AsyncLoadResult{
            .reload = ReloadCheck{ .decision = { c.reload_op, c.reload_diff_pos }, .base = base } });
        // worker は Replace ならパース済みの文書を同じパスで返す。
        if (c.reload_op == ReloadOp::Replace) {
            (*f.outcome)->doc = Document::FromMarkdown(std::string(c.result_text), c.current_path);
        }
        break;
    }
    }
    return f;
}

class PlanParseCompleteTest : public ::testing::TestWithParam<Case> {};

} // namespace

TEST_P(PlanParseCompleteTest, ChoosesStep)
{
    const Case& c = GetParam();
    Fixture f = Build(c);
    ASSERT_EQ(f.current.ShareData() != nullptr, !c.current_path.empty() || !c.current_text.empty());

    EXPECT_EQ(PlanParseComplete(f.outcome, f.current), c.step);
}

INSTANTIATE_TEST_SUITE_P(
    Table, PlanParseCompleteTest,
    ::testing::Values(
        // ---- 結果なし ----
        // 次の読み込みで捨てた結果も、取り消した結果も、後始末は捨てた側が済ませている。
        Case{ .label = "DroppedResultIsStale", .step = ParseCompleteStep::IgnoreStale },
        Case{ .label = "ErrorNotFound", .error = FileLoadError::NotFound, .step = ParseCompleteStep::Fail },
        Case{ .label = "ErrorTooLarge", .error = FileLoadError::TooLarge, .step = ParseCompleteStep::Fail },
        // 表示中でないファイルが使用中なら、読み直さずに知らせる (表示中のファイルは worker が Locked の判定にする)。
        Case{ .label = "ErrorLocked", .error = FileLoadError::Locked, .step = ParseCompleteStep::Fail },

        // ---- worker 差分判定済みのリロード ----
        Case{ .label = "ReloadNoChange", .kind = ResultKind::Reload, .reload_op = ReloadOp::NoChange,
              .step = ParseCompleteStep::ReloadUnchanged },
        Case{ .label = "ReloadDeferPrefixShrink", .kind = ResultKind::Reload, .reload_op = ReloadOp::DeferPrefixShrink, .reload_diff_pos = 7,
              .step = ParseCompleteStep::DeferPrefixShrink },
        Case{ .label = "ReloadReplace", .kind = ResultKind::Reload, .reload_op = ReloadOp::Replace, .reload_diff_pos = 9,
              .step = ParseCompleteStep::ApplyReload },
        Case{ .label = "ReloadStillGrowingRetries", .kind = ResultKind::Reload, .reload_op = ReloadOp::StillGrowing,
              .step = ParseCompleteStep::Retry },
        Case{ .label = "ReloadLockedRetries", .kind = ResultKind::Reload, .reload_op = ReloadOp::Locked,
              .step = ParseCompleteStep::RetryLocked },
        Case{ .label = "ReloadLockedAfterBaseReplacedRetries", .kind = ResultKind::Reload, .reload_op = ReloadOp::Locked, .base = BaseRef::Other,
              .step = ParseCompleteStep::Retry },
        Case{ .label = "ReloadBaseReplacedRetries", .kind = ResultKind::Reload, .reload_op = ReloadOp::NoChange, .base = BaseRef::Other,
              .step = ParseCompleteStep::Retry },
        Case{ .label = "ReloadBaseExpiredRetries", .kind = ResultKind::Reload, .reload_op = ReloadOp::Replace, .reload_diff_pos = 3,
              .base = BaseRef::Expired, .step = ParseCompleteStep::Retry },
        // 文書が空に差し替わった後に、旧文書が解放済みの判定結果が届いた。
        Case{ .label = "ReloadAfterSwitchToEmptyDocRetries", .current_path = L"", .current_text = "", .kind = ResultKind::Reload,
              .reload_op = ReloadOp::NoChange, .base = BaseRef::Expired, .step = ParseCompleteStep::Retry },

        // ---- 別ファイル ----
        Case{ .label = "OtherPathOpensNewDocument", .kind = ResultKind::Doc, .result_path = kOtherPath,
              .step = ParseCompleteStep::ReplaceWithResult },
        Case{ .label = "FirstDocumentOpensNewDocument", .current_path = L"", .current_text = "", .kind = ResultKind::Doc,
              .step = ParseCompleteStep::ReplaceWithResult }),
    [](const ::testing::TestParamInfo<Case>& info) static { return std::string(info.param.label); });

// 表示中のファイルは、変更の読み直しでもダイアログや D&D での開き直しでも worker に差分を判定させる。
TEST(ReloadBaseFor, OnlyForTheShownFile)
{
    const Document current = Document::FromMarkdown(std::string(kText), kPath);
    for (const bool accept : { false, true }) {
        const auto base = ReloadBaseFor(current, kPath, accept);
        EXPECT_EQ(base.data, current.ShareData());
        EXPECT_EQ(base.accept_prefix_shrink, accept);
    }
    EXPECT_EQ(ReloadBaseFor(current, kPathOtherCase, false).data, current.ShareData()) << "パスの大小は区別しない";
    EXPECT_EQ(ReloadBaseFor(current, kOtherPath, false).data, nullptr);
    EXPECT_EQ(ReloadBaseFor(Document{}, kPath, false).data, nullptr) << "文書が無い (起動直後)";
}

// ---- ReloadScheduler ----

namespace {

using Step = ParseCompleteStep;
using RS = ReloadScheduler;

constexpr UINT kDebounceMs = app_timer::FILE_RELOAD_DEBOUNCE_MS;
constexpr UINT kRetryMs = app_timer::FILE_RELOAD_RETRY_MS;
constexpr UINT kLockedMs = app_timer::FILE_RELOAD_LOCKED_RETRY_MS;

template <class T>
bool In(const ReloadScheduler& s)
{
    return std::holds_alternative<T>(s.GetState());
}

// 変更の通知から読み直しを始めたところ。
ReloadScheduler StartedReload()
{
    ReloadScheduler s;
    (void)s.OnFileChanged();
    EXPECT_TRUE(s.OnTimer(false).start_reload);
    return s;
}

} // namespace

TEST(ReloadScheduler, DebouncesFromFirstNotificationThenReloads)
{
    ReloadScheduler s;
    auto cmd = s.OnFileChanged();
    EXPECT_EQ(cmd.set_timer_ms, kDebounceMs);
    EXPECT_FALSE(cmd.start_reload);
    // 続けて保存されても掛け直さない。
    cmd = s.OnFileChanged();
    EXPECT_EQ(cmd.set_timer_ms, 0u);
    EXPECT_FALSE(cmd.kill_timer);

    cmd = s.OnTimer(false);
    EXPECT_TRUE(cmd.start_reload);
    EXPECT_TRUE(cmd.kill_timer);
    EXPECT_TRUE(In<RS::Reloading>(s));

    cmd = s.OnLoadFinished(Step::ReloadUnchanged, false);
    EXPECT_TRUE(In<RS::Idle>(s));
    EXPECT_EQ(cmd.set_timer_ms, 0u);
    EXPECT_FALSE(cmd.kill_timer);
}

// 読んでいる間に届いた変更は、読み終えてから最初の通知として待ち直す。
TEST(ReloadScheduler, ChangeDuringReloadWaitsAgain)
{
    for (const Step step : { Step::ReloadUnchanged, Step::ApplyReload, Step::Fail }) {
        auto s = StartedReload();
        EXPECT_EQ(s.OnFileChanged().set_timer_ms, 0u);
        const auto cmd = s.OnLoadFinished(step, false);
        EXPECT_EQ(cmd.set_timer_ms, kDebounceMs);
        EXPECT_EQ(cmd.apply_result, step == Step::ApplyReload);
    }
}

// 書き込みの途中を読んだら短い間隔で読み直す。それまでの変更はその読み込みに含まれるので、もう一度は読まない。
TEST(ReloadScheduler, RetryAfterShortInterval)
{
    auto s = StartedReload();
    (void)s.OnFileChanged();
    EXPECT_EQ(s.OnLoadFinished(Step::Retry, false).set_timer_ms, kRetryMs);
    EXPECT_EQ(s.OnFileChanged().set_timer_ms, 0u) << "待つ間の変更で延ばさない";
    EXPECT_TRUE(s.OnTimer(false).start_reload);
    EXPECT_EQ(s.OnLoadFinished(Step::ReloadUnchanged, false).set_timer_ms, 0u);
    EXPECT_TRUE(In<RS::Idle>(s));
}

TEST(ReloadScheduler, LockedFileNotifiesOnceThenWaitsLonger)
{
    auto s = StartedReload();
    for (int i = 1; i < RS::kLockedRetriesBeforeNotice; ++i) {
        const auto cmd = s.OnLoadFinished(Step::RetryLocked, false);
        ASSERT_FALSE(cmd.notify_locked) << i;
        ASSERT_EQ(cmd.set_timer_ms, kRetryMs) << i;
        ASSERT_TRUE(s.OnTimer(false).start_reload);
    }
    auto cmd = s.OnLoadFinished(Step::RetryLocked, false);
    EXPECT_TRUE(cmd.notify_locked);
    EXPECT_EQ(cmd.set_timer_ms, kLockedMs);
    ASSERT_TRUE(s.OnTimer(false).start_reload);
    cmd = s.OnLoadFinished(Step::RetryLocked, false);
    EXPECT_FALSE(cmd.notify_locked);
    EXPECT_EQ(cmd.set_timer_ms, kLockedMs);
}

// 開けても書き込みの途中だったら、開けなかった回数は数え直す。
TEST(ReloadScheduler, LockedCountRestartsAfterOtherResult)
{
    auto s = StartedReload();
    for (int i = 1; i < RS::kLockedRetriesBeforeNotice; ++i) {
        (void)s.OnLoadFinished(Step::RetryLocked, false);
        (void)s.OnTimer(false);
    }
    (void)s.OnLoadFinished(Step::Retry, false);
    (void)s.OnTimer(false);
    EXPECT_FALSE(s.OnLoadFinished(Step::RetryLocked, false).notify_locked);
}

// 末尾を削っただけの保存は上限まで defer し、超えたら縮んだ内容を受け入れる。
TEST(ReloadScheduler, PrefixShrinkAcceptedAfterLimit)
{
    auto s = StartedReload();
    for (int i = 0; i < RS::kMaxPrefixShrinkDefers; ++i) {
        ASSERT_FALSE(s.AcceptPrefixShrink()) << i;
        ASSERT_EQ(s.OnLoadFinished(Step::DeferPrefixShrink, false).set_timer_ms, kRetryMs);
        ASSERT_TRUE(s.OnTimer(false).start_reload);
    }
    EXPECT_TRUE(s.AcceptPrefixShrink());
    (void)s.OnLoadFinished(Step::ApplyReload, false);
    EXPECT_FALSE(s.AcceptPrefixShrink());
}

// 読み直しを終えたら (失敗を含む)、次の変更の読み直しに defer の回数を持ち越さない。
TEST(ReloadScheduler, PrefixShrinkCountEndsWithReload)
{
    for (const Step end : { Step::Fail, Step::ReloadUnchanged, Step::ApplyReload }) {
        auto s = StartedReload();
        for (int i = 0; i < RS::kMaxPrefixShrinkDefers; ++i) {
            (void)s.OnLoadFinished(Step::DeferPrefixShrink, false);
            (void)s.OnTimer(false);
        }
        ASSERT_TRUE(s.AcceptPrefixShrink());
        (void)s.OnLoadFinished(end, false);
        (void)s.OnFileChanged();
        ASSERT_TRUE(s.OnTimer(false).start_reload);
        EXPECT_FALSE(s.AcceptPrefixShrink());
    }
}

// テキスト選択のドラッグ中は差し替えず、終わるまで短い間隔で待つ。
TEST(ReloadScheduler, WaitsWhileSelecting)
{
    ReloadScheduler s;
    (void)s.OnFileChanged();
    auto cmd = s.OnTimer(true);
    EXPECT_FALSE(cmd.start_reload);
    EXPECT_EQ(cmd.set_timer_ms, kRetryMs);
    // 同じ間隔で待ち続けるなら掛け直さない (Win32 のタイマーは繰り返す)。
    cmd = s.OnTimer(true);
    EXPECT_FALSE(cmd.start_reload);
    EXPECT_EQ(cmd.set_timer_ms, 0u);
    EXPECT_FALSE(cmd.kill_timer);
    EXPECT_TRUE(s.OnTimer(false).start_reload);

    // 読み始めた後にドラッグが始まったら、結果を捨てて読み直す。
    cmd = s.OnLoadFinished(Step::ApplyReload, true);
    EXPECT_FALSE(cmd.apply_result);
    EXPECT_EQ(cmd.set_timer_ms, kRetryMs);
}

// F5 は待っていた読み直しを今始め、タイマーを止める (後でもう一度読まない)。読み込み中は重ねない。
TEST(ReloadScheduler, ManualReloadTakesOverWaiting)
{
    ReloadScheduler s;
    (void)s.OnFileChanged();
    const auto cmd = s.OnReloadRequested(false);
    EXPECT_TRUE(cmd.start_reload);
    EXPECT_TRUE(cmd.kill_timer);
    EXPECT_FALSE(s.OnReloadRequested(false).start_reload);

    ReloadScheduler other;
    (void)other.OnLoadStarted(false);
    EXPECT_FALSE(other.OnReloadRequested(false).start_reload);
}

// 別のファイルの読み込み中に届いた変更は、その読み込みが文書を差し替えずに終わったら読み直す。
TEST(ReloadScheduler, ChangeDuringOtherLoadReloadsUnlessReplaced)
{
    for (const bool cancel : { false, true }) {
        ReloadScheduler s;
        EXPECT_EQ(s.OnLoadStarted(false).set_timer_ms, 0u);
        ASSERT_TRUE(s.IsLoadingOther());
        EXPECT_EQ(s.OnFileChanged().set_timer_ms, 0u);
        const auto cmd = cancel ? s.OnOtherLoadCancelled() : s.OnLoadFinished(Step::Fail, false);
        EXPECT_EQ(cmd.set_timer_ms, kDebounceMs) << cancel;
    }
    ReloadScheduler s;
    (void)s.OnLoadStarted(false);
    (void)s.OnFileChanged();
    EXPECT_EQ(s.OnLoadFinished(Step::ReplaceWithResult, false).set_timer_ms, 0u);
    EXPECT_TRUE(In<RS::Idle>(s));
}

// 別のファイルを開き始めると、待っていた読み直しも取り消される読み直しも、差し替えずに終わるまで待たせる。
TEST(ReloadScheduler, OtherLoadHoldsPendingReload)
{
    ReloadScheduler waiting;
    (void)waiting.OnFileChanged();
    EXPECT_TRUE(waiting.OnLoadStarted(false).kill_timer);
    EXPECT_EQ(waiting.OnOtherLoadCancelled().set_timer_ms, kDebounceMs);

    auto reloading = StartedReload();
    (void)reloading.OnLoadStarted(false);
    EXPECT_EQ(reloading.OnLoadFinished(Step::Fail, false).set_timer_ms, kDebounceMs);

    ReloadScheduler idle;
    (void)idle.OnLoadStarted(false);
    EXPECT_EQ(idle.OnOtherLoadCancelled().set_timer_ms, 0u);
    EXPECT_TRUE(In<RS::Idle>(idle));
}

// 表示中のファイルを開き直すと、待っていた読み直しの代わりになる。読み込みは開く側が始める。
TEST(ReloadScheduler, ReopeningShownFileIsReload)
{
    ReloadScheduler s;
    (void)s.OnFileChanged();
    const auto cmd = s.OnLoadStarted(true);
    EXPECT_TRUE(cmd.kill_timer);
    EXPECT_FALSE(cmd.start_reload);
    EXPECT_TRUE(In<RS::Reloading>(s));
    EXPECT_FALSE(s.IsLoadingOther());
}

TEST(ReloadScheduler, DocumentReplacedStopsWaiting)
{
    ReloadScheduler s;
    (void)s.OnFileChanged();
    EXPECT_TRUE(s.OnDocumentReplaced().kill_timer);
    EXPECT_TRUE(In<RS::Idle>(s));
    // 止めた後に届いたタイマーは止めるだけ。
    const auto cmd = s.OnTimer(false);
    EXPECT_TRUE(cmd.kill_timer);
    EXPECT_FALSE(cmd.start_reload);
}

// どの順で入力が来ても、タイマーは Waiting の間だけその間隔で掛かり、読み込み中なのは Reloading と LoadingOther の間だけ。
TEST(ReloadScheduler, TimerAndLoadFollowStateUnderRandomEvents)
{
    static constexpr Step kOtherResults[] = { Step::Fail, Step::ReplaceWithResult };
    static constexpr Step kReloadResults[] = { Step::Fail, Step::Retry, Step::RetryLocked, Step::ReloadUnchanged, Step::DeferPrefixShrink, Step::ApplyReload };
    std::mt19937 rng(12345);
    for (int run = 0; run < 200; ++run) {
        ReloadScheduler s;
        UINT armed = 0;
        bool loading = false;
        for (int i = 0; i < 60; ++i) {
            const bool selecting = rng() % 4 == 0;
            ReloadCommand cmd;
            switch (rng() % 8) {
            case 0:
                cmd = s.OnFileChanged();
                break;
            case 1:
                // 掛かっていないタイマーは鳴らない。
                if (armed == 0) {
                    continue;
                }
                cmd = s.OnTimer(selecting);
                break;
            case 2:
                cmd = s.OnReloadRequested(selecting);
                break;
            case 3:
                cmd = s.OnLoadStarted(rng() % 2 == 0);
                loading = true;
                break;
            case 4:
                if (s.IsLoadingOther()) {
                    loading = false;
                }
                cmd = s.OnOtherLoadCancelled();
                break;
            case 5:
                cmd = s.OnDocumentReplaced();
                loading = false;
                break;
            default: {
                if (!loading) {
                    continue;
                }
                const Step step = s.IsLoadingOther() ? kOtherResults[rng() % std::size(kOtherResults)] : kReloadResults[rng() % std::size(kReloadResults)];
                cmd = s.OnLoadFinished(step, selecting);
                loading = false;
                break;
            }
            }
            if (cmd.kill_timer) {
                armed = 0;
            }
            if (cmd.set_timer_ms != 0) {
                armed = cmd.set_timer_ms;
            }
            if (cmd.start_reload) {
                ASSERT_TRUE(In<RS::Reloading>(s));
                loading = true;
            }
            const auto* w = std::get_if<RS::Waiting>(&s.GetState());
            const UINT expected = !w                              ? 0u
                                  : w->wait == RS::Wait::Debounce ? kDebounceMs
                                  : w->wait == RS::Wait::Retry    ? kRetryMs
                                                                  : kLockedMs;
            ASSERT_EQ(armed, expected) << "run " << run << " event " << i;
            ASSERT_EQ(loading, In<RS::Reloading>(s) || In<RS::LoadingOther>(s)) << "run " << run << " event " << i;
        }
    }
}
