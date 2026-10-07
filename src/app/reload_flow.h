#pragma once
// 非同期ロード完了 (PARSE_COMPLETE) 時の分岐判定。App::OnParseComplete は Win32/描画に依存して
// 単体テストできないため、どの処理を取るかの決定だけを純粋関数に切り出し、App は計画を実行するだけにする。
#include "async_load_result.h"
#include "document.h"
#include "file_io.h"
#include "file_loader.h"
#include "reload.h"
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
    // 結果もエラーも無いのに別のロードが進行中 = Start の ResetSinks で無効化された古い通知。
    // 進行中ロードのアニメーションや FileWatcher 状態に触らない。
    IgnoreStale,
    // 結果なし (失敗 / キャンセル)。paused の FileWatcher を必ず再開させる。
    Fail,
    // worker の差分判定後に表示文書が差し替わり、判定の前提が崩れた。
    RetryDocumentChanged,
    // エディタの書き込み途中を読んだ (読み込み後にファイルが伸びている)。
    RetryPartialWrite,
    // 同一ファイルの再読込で、FullReload 以外 (NoChange / DeferPrefixShrink / PrefixGrowth)。op は reload に入る。
    ApplyReload,
    // 結果の文書へ差し替える (別ファイルのロード、または同一ファイルの FullReload)。
    ReplaceWithResult,
};

struct ParseCompletePlan {
    ParseCompleteStep step = ParseCompleteStep::IgnoreStale;
    // Fail のときのトースト表示用。キャンセル由来なら空。
    std::optional<FileLoadError> error;
    // 同一ファイルの再読込と判定したときの差分判定。ApplyReload と ReplaceWithResult (FullReload) で設定される。
    std::optional<ReloadDecision> reload;
    // ReplaceWithResult のときのみ意味を持つ。
    bool follow_file_pane = false;
};

// result / error は FileLoadService から取り出した直後の値 (result があれば error は見ない)。
// another_load_active は取り出し後の IsAsyncLoading()。
// is_partial_write(path, read_size) は同一パスの再読込のときだけ呼ばれる (ファイルサイズを問い合わせる I/O)。
template <class IsPartialWrite>
[[nodiscard]] ParseCompletePlan PlanParseComplete(const AsyncLoadResult* result, std::optional<FileLoadError> error,
                                                  bool another_load_active, const Document& current,
                                                  IsPartialWrite&& is_partial_write)
{
    if (!result) {
        if (!error && another_load_active) {
            return { .step = ParseCompleteStep::IgnoreStale };
        }
        return { .step = ParseCompleteStep::Fail, .error = error };
    }

    // 差分ベースのスキップ／スクロール復元は同一パスのリロード時のみ有効。
    // 非同期のファイルオープンでも同じ経路を通るため、別ファイルなら decision は空のまま。
    std::optional<ReloadDecision> decision;
    if (result->reload) {
        // worker がパース前に差分判定を済ませている (NoChange 等は doc が空)。
        const auto& rc = *result->reload;
        if (rc.base.lock() != current.GetRawText().Share() || !path_util::iequal(rc.path, current.GetFilePath())) {
            return { .step = ParseCompleteStep::RetryDocumentChanged };
        }
        if (is_partial_write(rc.path, rc.loaded_byte_size)) {
            return { .step = ParseCompleteStep::RetryPartialWrite };
        }
        decision = rc.decision;
    }
    else if (path_util::iequal(result->doc.GetFilePath(), current.GetFilePath())) {
        if (is_partial_write(result->doc.GetFilePath(), result->doc.GetLoadedByteSize())) {
            return { .step = ParseCompleteStep::RetryPartialWrite };
        }
        decision = AnalyzeReloadDiff(std::string_view(current.GetRawText()), std::string_view(result->doc.GetRawText()));
    }

    if (decision && decision->op != ReloadOp::FullReload) {
        return { .step = ParseCompleteStep::ApplyReload, .reload = decision };
    }
    return { .step = ParseCompleteStep::ReplaceWithResult,
             .reload = decision,
             .follow_file_pane = FilePaneFollowsLoad(current.GetFilePath(), result->doc.GetFilePath()) };
}
