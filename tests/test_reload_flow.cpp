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
// パスの大小違い、書き込み途中の延期、差分判定の各結果。

namespace {

constexpr std::wstring_view kPath = L"C:\\docs\\note.md";
constexpr std::wstring_view kPathOtherCase = L"c:\\DOCS\\NOTE.MD";
constexpr std::wstring_view kOtherPath = L"C:\\docs\\other.md";
constexpr std::string_view kText = "# Title\n\nbody\n";

enum class ResultKind : uint8_t {
    None,
    // worker が差分判定前に返した通常のロード結果 (reload_base なしで Start された)。
    Doc,
    // reload_base 付きで Start され、worker が差分判定を済ませた結果。
    Reload,
};

enum class BaseRef : uint8_t {
    Current, // 表示中の文書のテキストそのもの
    Other,   // 別のテキスト (判定後に表示文書が差し替わった)
    Expired, // 旧テキストは既に解放済み
};

struct Case {
    const char* label;
    // 表示中の文書
    std::wstring_view current_path = kPath;
    std::string_view current_text = kText;
    // FileLoadService から取り出した値
    ResultKind kind = ResultKind::None;
    std::optional<FileLoadError> error;
    bool another_load_active = false;
    // Doc
    std::wstring_view result_path = kPath;
    std::string_view result_text = kText;
    // Reload
    ReloadOp reload_op = ReloadOp::NoChange;
    size_t reload_diff_pos = std::string_view::npos;
    BaseRef base = BaseRef::Current;
    std::wstring_view reload_path = kPath;
    // 書き込み途中判定の戻り値
    bool partial_write = false;

    // 期待値
    ParseCompleteStep step;
    std::optional<ReloadOp> expect_op = std::nullopt;
    size_t expect_diff_pos = std::string_view::npos;
    bool expect_follow = false;
    // 書き込み途中判定が呼ばれるべきか (呼ばれるなら引数はケースから導出して検査する)
    bool expect_probe = false;
};

void PrintTo(const Case& c, std::ostream* os)
{
    *os << c.label;
}

constexpr size_t kReloadByteSize = 4242;

struct Fixture {
    Document current;
    std::shared_ptr<const std::pmr::string> other_base;
    std::optional<AsyncLoadResult> result;
};

Fixture Build(const Case& c)
{
    Fixture f;
    if (!c.current_path.empty() || !c.current_text.empty()) {
        f.current = Document::FromMarkdown(std::pmr::string(c.current_text), c.current_path);
    }
    switch (c.kind) {
    case ResultKind::None:
        break;
    case ResultKind::Doc:
        f.result.emplace(AsyncLoadResult{ Document::FromMarkdown(std::pmr::string(c.result_text), c.result_path) });
        break;
    case ResultKind::Reload: {
        std::weak_ptr<const std::pmr::string> base;
        switch (c.base) {
        case BaseRef::Current:
            base = f.current.GetRawText().Share();
            break;
        case BaseRef::Other:
            f.other_base = std::make_shared<const std::pmr::string>(c.current_text);
            base = f.other_base;
            break;
        case BaseRef::Expired:
            base = std::make_shared<const std::pmr::string>(c.current_text);
            break;
        }
        f.result.emplace(AsyncLoadResult{
            .reload = ReloadCheck{ ReloadDecision{ c.reload_op, c.reload_diff_pos }, base, std::pmr::wstring(c.reload_path), kReloadByteSize } });
        // worker は NoChange / DeferPrefixShrink 以外ならパース済みの文書を同じパスで返す。
        if (c.reload_op == ReloadOp::PrefixGrowth || c.reload_op == ReloadOp::FullReload) {
            f.result->doc = Document::FromMarkdown(std::pmr::string(c.result_text), c.reload_path);
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
    ASSERT_EQ(f.current.GetRawText().Share() != nullptr, !c.current_text.empty());

    std::vector<std::pair<std::wstring, size_t>> probes;
    const auto plan = PlanParseComplete(f.result ? &*f.result : nullptr, c.error, c.another_load_active, f.current,
                                        [&](const std::pmr::wstring& path, size_t read_size) {
                                            probes.emplace_back(std::wstring(std::wstring_view(path)), read_size);
                                            return c.partial_write;
                                        });

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

    if (!c.expect_probe) {
        EXPECT_TRUE(probes.empty()) << "同一パスの再読込以外でファイルサイズを問い合わせない";
    }
    else {
        ASSERT_EQ(probes.size(), 1u);
        // 判定に使ったのと同じ読み込みのパスとバイト数で問い合わせる。
        if (c.kind == ResultKind::Reload) {
            EXPECT_EQ(probes[0].first, c.reload_path);
            EXPECT_EQ(probes[0].second, kReloadByteSize);
        }
        else {
            EXPECT_EQ(probes[0].first, c.result_path);
            EXPECT_EQ(probes[0].second, f.result->doc.GetLoadedByteSize());
        }
    }
}

INSTANTIATE_TEST_SUITE_P(
    Table, PlanParseCompleteTest,
    ::testing::Values(
        // ---- 結果なし ----
        Case{ .label = "StaleWhileAnotherLoadActive", .another_load_active = true, .step = ParseCompleteStep::IgnoreStale },
        Case{ .label = "CancelledWithoutNewLoad", .step = ParseCompleteStep::Fail },
        Case{ .label = "ErrorIsReportedEvenIfAnotherLoadActive", .error = FileLoadError::NotFound, .another_load_active = true,
              .step = ParseCompleteStep::Fail },
        Case{ .label = "ErrorTooLarge", .error = FileLoadError::TooLarge, .step = ParseCompleteStep::Fail },

        // ---- worker 差分判定済みのリロード ----
        Case{ .label = "ReloadNoChange", .kind = ResultKind::Reload, .reload_op = ReloadOp::NoChange,
              .step = ParseCompleteStep::ApplyReload, .expect_op = ReloadOp::NoChange, .expect_probe = true },
        Case{ .label = "ReloadDeferPrefixShrink", .kind = ResultKind::Reload, .reload_op = ReloadOp::DeferPrefixShrink, .reload_diff_pos = 7,
              .step = ParseCompleteStep::ApplyReload, .expect_op = ReloadOp::DeferPrefixShrink, .expect_diff_pos = 7,
              .expect_probe = true },
        Case{ .label = "ReloadPrefixGrowth", .kind = ResultKind::Reload, .reload_op = ReloadOp::PrefixGrowth, .reload_diff_pos = 14,
              .step = ParseCompleteStep::ApplyReload, .expect_op = ReloadOp::PrefixGrowth, .expect_diff_pos = 14,
              .expect_probe = true },
        Case{ .label = "ReloadFullReloadReplacesWithoutFollowingPane", .kind = ResultKind::Reload, .reload_op = ReloadOp::FullReload,
              .reload_diff_pos = 9, .step = ParseCompleteStep::ReplaceWithResult, .expect_op = ReloadOp::FullReload,
              .expect_diff_pos = 9, .expect_probe = true },
        Case{ .label = "ReloadPathCaseDiffersStillSameFile", .kind = ResultKind::Reload, .reload_op = ReloadOp::NoChange,
              .reload_path = kPathOtherCase, .step = ParseCompleteStep::ApplyReload, .expect_op = ReloadOp::NoChange,
              .expect_probe = true },
        Case{ .label = "ReloadPartialWriteRetries", .kind = ResultKind::Reload, .reload_op = ReloadOp::FullReload, .reload_diff_pos = 9,
              .partial_write = true, .step = ParseCompleteStep::RetryPartialWrite, .expect_probe = true },
        Case{ .label = "ReloadBaseReplacedRetries", .kind = ResultKind::Reload, .reload_op = ReloadOp::NoChange, .base = BaseRef::Other,
              .step = ParseCompleteStep::RetryDocumentChanged },
        Case{ .label = "ReloadBaseExpiredRetries", .kind = ResultKind::Reload, .reload_op = ReloadOp::FullReload, .reload_diff_pos = 3,
              .base = BaseRef::Expired, .step = ParseCompleteStep::RetryDocumentChanged },
        // 同じテキストのまま別ファイルを開いた (base は一致するがパスが違う)。
        Case{ .label = "ReloadForOtherPathRetries", .kind = ResultKind::Reload, .reload_op = ReloadOp::NoChange, .reload_path = kOtherPath,
              .step = ParseCompleteStep::RetryDocumentChanged },
        // 文書が空 (raw_text 共有なし) に差し替わった後に、旧テキストが解放済みの判定結果が届いた。
        Case{ .label = "ReloadAfterSwitchToEmptyDocRetries", .current_path = L"", .current_text = "", .kind = ResultKind::Reload,
              .reload_op = ReloadOp::NoChange, .base = BaseRef::Expired, .step = ParseCompleteStep::RetryDocumentChanged },

        // ---- 通常ロード結果が同一パス (UI 側で差分判定) ----
        Case{ .label = "SamePathUnchanged", .kind = ResultKind::Doc, .step = ParseCompleteStep::ApplyReload,
              .expect_op = ReloadOp::NoChange, .expect_probe = true },
        Case{ .label = "SamePathPrefixGrowth", .kind = ResultKind::Doc, .result_text = "# Title\n\nbody\nmore\n",
              .step = ParseCompleteStep::ApplyReload, .expect_op = ReloadOp::PrefixGrowth, .expect_diff_pos = kText.size(),
              .expect_probe = true },
        Case{ .label = "SamePathPrefixShrinkDefers", .kind = ResultKind::Doc, .result_text = "# Title\n",
              .step = ParseCompleteStep::ApplyReload, .expect_op = ReloadOp::DeferPrefixShrink, .expect_diff_pos = 8,
              .expect_probe = true },
        Case{ .label = "SamePathOtherCaseFullReload", .kind = ResultKind::Doc, .result_path = kPathOtherCase,
              .result_text = "# Title\n\nBODY\n", .step = ParseCompleteStep::ReplaceWithResult, .expect_op = ReloadOp::FullReload,
              .expect_diff_pos = 9, .expect_follow = false, .expect_probe = true },
        Case{ .label = "SamePathPartialWriteRetries", .kind = ResultKind::Doc, .result_text = "# Title\n\nchanged\n", .partial_write = true,
              .step = ParseCompleteStep::RetryPartialWrite, .expect_probe = true },

        // ---- 別ファイル ----
        Case{ .label = "OtherPathOpensNewDocumentAndFollowsPane", .kind = ResultKind::Doc, .result_path = kOtherPath,
              .step = ParseCompleteStep::ReplaceWithResult, .expect_follow = true },
        // 別ファイルなら書き込み途中判定は不要 (同一パスの再読込に限る)。
        Case{ .label = "OtherPathIgnoresPartialWrite", .kind = ResultKind::Doc, .result_path = kOtherPath, .partial_write = true,
              .step = ParseCompleteStep::ReplaceWithResult, .expect_follow = true },
        Case{ .label = "FirstDocumentDoesNotFollowPane", .current_path = L"", .current_text = "", .kind = ResultKind::Doc,
              .step = ParseCompleteStep::ReplaceWithResult, .expect_follow = false }),
    [](const ::testing::TestParamInfo<Case>& info) { return std::string(info.param.label); });

TEST(FilePaneFollowsLoad, OnlyWhenSwitchingFromAnExistingDocumentToAnotherFile)
{
    EXPECT_FALSE(FilePaneFollowsLoad(L"", L"C:\\a.md"));
    EXPECT_FALSE(FilePaneFollowsLoad(L"C:\\a.md", L"C:\\a.md"));
    EXPECT_FALSE(FilePaneFollowsLoad(L"C:\\a.md", L"c:\\A.MD"));
    EXPECT_TRUE(FilePaneFollowsLoad(L"C:\\a.md", L"C:\\b.md"));
}
