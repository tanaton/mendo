#include <gtest/gtest.h>
#include "reload_flow.h"
#include <memory>
#include <optional>
#include <ostream>
#include <string>
#include <string_view>
#include <utility>
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
    // worker が結果を出す直前に、ファイルがまだ伸びていた
    bool still_growing = false;

    // 期待値
    ParseCompleteStep step;
    std::optional<ReloadOp> expect_op = std::nullopt;
    size_t expect_diff_pos = std::string_view::npos;
    bool expect_follow = false;
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
        f.current = Document::FromMarkdown(std::pmr::string(c.current_text), c.current_path);
    }
    switch (c.kind) {
    case ResultKind::None:
        if (c.error) {
            f.outcome.emplace(std::unexpected(*c.error));
        }
        break;
    case ResultKind::Doc:
        f.outcome.emplace(AsyncLoadResult{ Document::FromMarkdown(std::pmr::string(c.result_text), c.result_path) });
        break;
    case ResultKind::Reload: {
        std::weak_ptr<const DocumentData> base;
        switch (c.base) {
        case BaseRef::Current:
            base = f.current.ShareData();
            break;
        case BaseRef::Other:
            f.other_base = Document::FromMarkdown(std::pmr::string(c.current_text), c.current_path).ShareData();
            base = f.other_base;
            break;
        case BaseRef::Expired:
            base = Document::FromMarkdown(std::pmr::string(c.current_text), c.current_path).ShareData();
            break;
        }
        f.outcome.emplace(AsyncLoadResult{
            .reload = ReloadCheck{ ReloadDecision{ c.reload_op, c.reload_diff_pos }, base } });
        // worker は NoChange / DeferPrefixShrink 以外ならパース済みの文書を同じパスで返す。
        if (c.reload_op == ReloadOp::Replace) {
            (*f.outcome)->doc = Document::FromMarkdown(std::pmr::string(c.result_text), c.current_path);
        }
        break;
    }
    }
    if (f.outcome && f.outcome->has_value()) {
        (*f.outcome)->still_growing = c.still_growing;
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

    const auto plan = PlanParseComplete(f.outcome, f.current);

    EXPECT_EQ(plan.step, c.step);
    if (c.step == ParseCompleteStep::Fail) {
        EXPECT_EQ(plan.error, c.error);
    }
    else {
        EXPECT_FALSE(plan.error.has_value());
    }
    ASSERT_EQ(plan.reload.has_value(), c.expect_op.has_value());
    if (c.expect_op) {
        EXPECT_EQ(plan.reload->op, *c.expect_op);
        EXPECT_EQ(plan.reload->diff_pos, c.expect_diff_pos);
    }
    EXPECT_EQ(plan.follow_file_pane, c.expect_follow);
}

INSTANTIATE_TEST_SUITE_P(
    Table, PlanParseCompleteTest,
    ::testing::Values(
        // ---- 結果なし ----
        // 次の読み込みで捨てた結果も、取り消した結果も、後始末は捨てた側が済ませている。
        Case{ .label = "DroppedResultIsStale", .step = ParseCompleteStep::IgnoreStale },
        Case{ .label = "ErrorNotFound", .error = FileLoadError::NotFound, .step = ParseCompleteStep::Fail },
        Case{ .label = "ErrorTooLarge", .error = FileLoadError::TooLarge, .step = ParseCompleteStep::Fail },

        // ---- worker 差分判定済みのリロード ----
        Case{ .label = "ReloadNoChange", .kind = ResultKind::Reload, .reload_op = ReloadOp::NoChange,
              .step = ParseCompleteStep::ApplyReload, .expect_op = ReloadOp::NoChange },
        Case{ .label = "ReloadDeferPrefixShrink", .kind = ResultKind::Reload, .reload_op = ReloadOp::DeferPrefixShrink, .reload_diff_pos = 7,
              .step = ParseCompleteStep::ApplyReload, .expect_op = ReloadOp::DeferPrefixShrink, .expect_diff_pos = 7 },
        Case{ .label = "ReloadReplace", .kind = ResultKind::Reload, .reload_op = ReloadOp::Replace, .reload_diff_pos = 9,
              .step = ParseCompleteStep::ApplyReload, .expect_op = ReloadOp::Replace, .expect_diff_pos = 9 },
        Case{ .label = "ReloadPartialWriteRetries", .kind = ResultKind::Reload, .reload_op = ReloadOp::Replace, .reload_diff_pos = 9,
              .still_growing = true, .step = ParseCompleteStep::Retry },
        // worker が書き込み途中を検出したときは判定せずに返す (doc も空)。
        Case{ .label = "ReloadPartialWriteWithoutDecisionRetries", .kind = ResultKind::Reload, .still_growing = true,
              .step = ParseCompleteStep::Retry },
        Case{ .label = "ReloadBaseReplacedRetries", .kind = ResultKind::Reload, .reload_op = ReloadOp::NoChange, .base = BaseRef::Other,
              .step = ParseCompleteStep::Retry },
        Case{ .label = "ReloadBaseExpiredRetries", .kind = ResultKind::Reload, .reload_op = ReloadOp::Replace, .reload_diff_pos = 3,
              .base = BaseRef::Expired, .step = ParseCompleteStep::Retry },
        // 文書が空に差し替わった後に、旧文書が解放済みの判定結果が届いた。
        Case{ .label = "ReloadAfterSwitchToEmptyDocRetries", .current_path = L"", .current_text = "", .kind = ResultKind::Reload,
              .reload_op = ReloadOp::NoChange, .base = BaseRef::Expired, .step = ParseCompleteStep::Retry },

        // ---- 別ファイル ----
        Case{ .label = "OtherPathOpensNewDocumentAndFollowsPane", .kind = ResultKind::Doc, .result_path = kOtherPath,
              .step = ParseCompleteStep::ReplaceWithResult, .expect_follow = true },
        Case{ .label = "FirstDocumentDoesNotFollowPane", .current_path = L"", .current_text = "", .kind = ResultKind::Doc,
              .step = ParseCompleteStep::ReplaceWithResult, .expect_follow = false }),
    [](const ::testing::TestParamInfo<Case>& info) static { return std::string(info.param.label); });

// 表示中のファイルは、ReloadCurrentFile でもダイアログや D&D での開き直しでも worker に差分を判定させる。
TEST(ReloadBaseFor, OnlyForTheShownFile)
{
    const Document current = Document::FromMarkdown(std::pmr::string(kText), kPath);
    for (const bool accept : { false, true }) {
        const auto base = ReloadBaseFor(current, kPath, accept);
        EXPECT_EQ(base.data, current.ShareData());
        EXPECT_EQ(base.accept_prefix_shrink, accept);
    }
    EXPECT_EQ(ReloadBaseFor(current, kPathOtherCase, false).data, current.ShareData()) << "パスの大小は区別しない";
    EXPECT_EQ(ReloadBaseFor(current, kOtherPath, false).data, nullptr);
    EXPECT_EQ(ReloadBaseFor(Document{}, kPath, false).data, nullptr) << "文書が無い (起動直後)";
}

TEST(FilePaneFollowsLoad, OnlyWhenSwitchingFromAnExistingDocumentToAnotherFile)
{
    EXPECT_FALSE(FilePaneFollowsLoad(L"", L"C:\\a.md"));
    EXPECT_FALSE(FilePaneFollowsLoad(L"C:\\a.md", L"C:\\a.md"));
    EXPECT_FALSE(FilePaneFollowsLoad(L"C:\\a.md", L"c:\\A.MD"));
    EXPECT_TRUE(FilePaneFollowsLoad(L"C:\\a.md", L"C:\\b.md"));
}
