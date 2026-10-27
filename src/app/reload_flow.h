#pragma once
// 非同期ロード完了 (PARSE_COMPLETE) 時の分岐判定。App::OnParseComplete は Win32/描画に依存して
// 単体テストできないため、どの処理を取るかの決定だけを純粋関数に切り出し、App は計画を実行するだけにする。
#include "async_load_coordinator.h"
#include "document.h"
#include "file_io.h"
#include "document_utils.h"
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

// 起動直後の最初の文書はペインのフォルダを起動引数から決めているため追従しない。
// 同じ文書の再読み込みも、ユーザーがペインで移動した先を保つため追従しない。
inline bool FilePaneFollowsLoad(std::wstring_view prev_path, std::wstring_view next_path) noexcept
{
    return !prev_path.empty() && !path_util::iequal(prev_path, next_path);
}

enum class ParseCompleteStep : uint8_t {
    // 結果もエラーも無い = Start の ResetSinks か Cancel で捨てた読み込みの古い通知。後始末は捨てた側
    // (Start は次の読み込み、Cancel の呼び出し元) が済ませているので、アニメーションや FileWatcher に触らない。
    IgnoreStale,
    // 読み込みに失敗した。paused の FileWatcher を必ず再開させる。
    Fail,
    // 読み直す。worker の差分判定の後に表示文書が差し替わって判定の前提が崩れたか、
    // エディタの書き込み途中を読んだ (読み込み後にファイルが伸びている)。
    Retry,
    // 同一ファイルの再読込。op は reload に入る。
    ApplyReload,
    // 別ファイルとして結果の文書を開く。
    ReplaceWithResult,
};

struct ParseCompletePlan {
    ParseCompleteStep step = ParseCompleteStep::IgnoreStale;
    // Fail のときのトースト表示用。キャンセル由来なら空。
    std::optional<FileLoadError> error;
    // ApplyReload のときの差分判定。
    std::optional<ReloadDecision> reload;
    // ReplaceWithResult のときのみ意味を持つ。
    bool follow_file_pane = false;
};

// outcome は AsyncLoadCoordinator::Take の戻り値。書き込み途中 (still_growing) は同一パスの再読込のときだけ見る。
[[nodiscard]] inline ParseCompletePlan PlanParseComplete(const std::optional<AsyncLoadOutcome>& outcome, const Document& current)
{
    if (!outcome) {
        return { .step = ParseCompleteStep::IgnoreStale };
    }
    if (!*outcome) {
        return { .step = ParseCompleteStep::Fail, .error = outcome->error() };
    }
    const AsyncLoadResult* const result = &**outcome;

    // 差分ベースのスキップ／スクロール復元は同一パスのリロード時のみ有効。
    // 非同期のファイルオープンでも同じ経路を通るため、別ファイルなら decision は空のまま。
    std::optional<ReloadDecision> decision;
    if (result->reload) {
        // worker がパース前に差分判定を済ませている (NoChange 等と書き込み途中は doc が空)。
        const auto& rc = *result->reload;
        if (result->still_growing || rc.base.lock() != current.ShareData() || !path_util::iequal(rc.path, current.GetFilePath())) {
            return { .step = ParseCompleteStep::Retry };
        }
        decision = rc.decision;
    }
    else if (path_util::iequal(result->doc.GetFilePath(), current.GetFilePath())) {
        if (result->still_growing) {
            return { .step = ParseCompleteStep::Retry };
        }
        decision = AnalyzeReloadDiff(std::string_view(current.GetRawText()), std::string_view(result->doc.GetRawText()));
    }

    if (decision) {
        return { .step = ParseCompleteStep::ApplyReload, .reload = decision };
    }
    return { .step = ParseCompleteStep::ReplaceWithResult,
             .follow_file_pane = FilePaneFollowsLoad(current.GetFilePath(), result->doc.GetFilePath()) };
}
