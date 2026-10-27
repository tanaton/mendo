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

// path を読むときに worker に渡す旧文書。表示中のファイルなら開き直しも含めてリロードとして扱い、
// 差分判定を worker に任せる (UI スレッドで取ると 100MB で約 20ms 止まる)。
[[nodiscard]] inline ReloadBase ReloadBaseFor(const Document& current, std::wstring_view path, bool accept_prefix_shrink)
{
    if (!path_util::iequal(path, current.GetFilePath())) {
        return {};
    }
    return { current.ShareData(), accept_prefix_shrink };
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
    // 同一ファイルの再読込。op は reload に、変わらなかったノード数は AsyncLoadResult::reload に入る。
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

// outcome は AsyncLoadCoordinator::Take の戻り値。
// 表示中のファイルの読み直しには必ず ReloadBase を付ける (ReloadBaseFor) ので、同じファイルかは worker の判定結果の有無で分かる。
[[nodiscard]] inline ParseCompletePlan PlanParseComplete(const std::optional<AsyncLoadOutcome>& outcome, const Document& current)
{
    if (!outcome) {
        return { .step = ParseCompleteStep::IgnoreStale };
    }
    if (!*outcome) {
        return { .step = ParseCompleteStep::Fail, .error = outcome->error() };
    }
    const AsyncLoadResult* const result = &**outcome;

    if (result->reload) {
        // worker がパース前に差分判定を済ませている (NoChange 等と書き込み途中は doc が空)。
        const auto& rc = *result->reload;
        // 解放済みの base は、文書が無い (ShareData が空) ときに一致と取り違えない。
        const auto base = rc.base.lock();
        if (result->still_growing || !base || base != current.ShareData()) {
            return { .step = ParseCompleteStep::Retry };
        }
        return { .step = ParseCompleteStep::ApplyReload, .reload = rc.decision };
    }
    return { .step = ParseCompleteStep::ReplaceWithResult,
             .follow_file_pane = FilePaneFollowsLoad(current.GetFilePath(), result->doc.GetFilePath()) };
}
