#include "async_load_coordinator.h"
#include "file_io.h"
#include "layout_computer.h"
#include "profiler.h"
#include "task_scheduler.h"
#include "utf8_codec.h"
#include <algorithm>
#include <cstring>
#include <limits>
#include <new>
#include <string_view>
#include <utility>

size_t FindFirstDifference(std::string_view old_text, std::string_view new_text) noexcept
{
    // MSVC STL の mismatch は 1 バイト要素をベクトル化する。
    const size_t pos = static_cast<size_t>(std::ranges::mismatch(old_text, new_text).in1 - old_text.begin());
    if (pos == old_text.size() && pos == new_text.size()) {
        return std::string_view::npos;
    }
    return pos;
}

ReloadDecision AnalyzeReloadDiff(std::string_view old_text, std::string_view new_text, bool accept_prefix_shrink) noexcept
{
    const size_t diff_pos = FindFirstDifference(old_text, new_text);
    if (diff_pos == std::string_view::npos) {
        return { ReloadOp::NoChange, std::string_view::npos };
    }
    // 新しい方が古い方の prefix (エディタの truncate→rewrite の途中)。
    if (diff_pos == new_text.size() && !accept_prefix_shrink) {
        return { ReloadOp::DeferPrefixShrink, diff_pos };
    }
    return { ReloadOp::Replace, diff_pos };
}

namespace {
// md4c が長さを int で取るため ~2GB-1 が実効上限。
constexpr LONGLONG MAX_FILE_SIZE = static_cast<LONGLONG>(std::numeric_limits<int>::max());
} // namespace

std::expected<LoadedFileDoc, FileLoadError> LoadFile(const std::wstring& path, const std::stop_token& /*stop_token*/)
{
    MENDO_PROFILE("LoadFile");
    // エディタがファイルを開いている間も読み取れるよう共有モードを許容。
    auto opened = OpenFileForReadShared(path.c_str(), path_util::kFileShareRWDelete, MAX_FILE_SIZE);
    if (!opened) {
        return std::unexpected(opened.error());
    }
    auto& r = *opened;

    if (r.size == 0) {
        return LoadedFileDoc{};
    }

    // 先頭 3 バイトを覗いて UTF-8 BOM (EF BB BF) ならスキップ、なければ string 先頭へ転記する。
    // 一括読み + erase(0,3) と違い BOM ありファイルでも全体スケールの memmove を発生させない。
    char prefix[3];
    const size_t prefix_read = std::min<size_t>(r.size, sizeof(prefix));
    if (!ReadExact(r.handle.get(), prefix, prefix_read)) {
        return std::unexpected(FileLoadError::ReadFailed);
    }

    const bool has_bom = std::string_view{ prefix, prefix_read } == utf8_codec::kBom;
    const size_t carry = has_bom ? 0u : prefix_read;
    const size_t remaining = r.size - prefix_read;
    const size_t out_size = carry + remaining;

    LoadedFileDoc result;
    result.byte_size = r.size;
    try {
        result.text.resize_and_overwrite(out_size, [&](char* buf, size_t /*cap*/) noexcept -> size_t {
            if (carry > 0) {
                std::memcpy(buf, prefix, carry);
            }
            if (remaining == 0) {
                return carry;
            }
            if (!ReadExact(r.handle.get(), buf + carry, remaining)) {
                return 0;
            }
            return carry + remaining;
        });
    } catch (const std::bad_alloc&) {
        return std::unexpected(FileLoadError::TooLarge);
    }
    if (result.text.size() != out_size) {
        return std::unexpected(FileLoadError::ReadFailed);
    }
    NormalizeNewlines(result.text);
    return result;
}

AsyncLoadCoordinator::~AsyncLoadCoordinator()
{
    Cancel();
    latch_.Wait();
}

void AsyncLoadCoordinator::ResetSinks() noexcept
{
    std::optional<AsyncLoadOutcome> stale;
    {
        const std::lock_guard lock(mutex_);
        stale.swap(outcome_);
    }
    // 推定済みの LayoutCache は D2D の参照を持たない。
    if (stale && *stale && scheduler_) {
        const size_t nodes = (*stale)->doc.GetNodes().size();
        DisposeOnWorkerIfLarge(*scheduler_, nodes, kDisposeMinNodes, std::move(stale));
    }
}

void AsyncLoadCoordinator::Start(TaskScheduler& scheduler, std::wstring path, Theme theme, float content_width, ReloadBase reload_base)
{
    // Cancel() と同じく旧 worker の stop を先に要求してから sink を空にし、旧 worker の publish を確実に弾く。
    // request_stop 後の source は再使用できないので作り直す。
    stop_source_.request_stop();
    stop_source_ = std::stop_source{};
    scheduler_ = &scheduler;
    ResetSinks();
    in_flight_ = true;
    loading_path_ = path;

    // 画像のデコードや図の読み込みが溜まっていても待たせない。リロードは旧文書の表示を続けるので、それらの待ちを捨てられない。
    const bool posted = scheduler.PostFront([this, path = std::move(path), theme = std::move(theme), content_width, stop_token = stop_source_.get_token(), reload_base = std::move(reload_base), guard = latch_.Acquire()]() mutable {
        try {
            RunWorker(path, theme, content_width, stop_token, std::move(reload_base));
        } catch (const std::bad_alloc&) {
            // 空の表の行補完などは入力の百倍以上を確保させられる。worker で投げると terminate するので失敗として返す。
            Publish(stop_token, std::unexpected(FileLoadError::TooLarge));
        }
    });

    if (!posted) {
        // Shutdown 後。lambda が走らないので latch::Guard は capture 内で
        // destruct され自動的に Release される。in_flight_/outcome_ を整えて UI に通知することで
        // 以降のリロードを再開可能にする。
        HWND hwnd = nullptr;
        UINT msg_id = 0;
        {
            const std::lock_guard lock(mutex_);
            outcome_.emplace(std::unexpected(FileLoadError::ReadFailed));
            in_flight_ = false;
            hwnd = notify_hwnd_;
            msg_id = notify_msg_;
        }
        if (hwnd) {
            ::PostMessageW(hwnd, msg_id, 0, 0);
        }
    }
}

bool AsyncLoadCoordinator::Attach(HWND hwnd, UINT msg_id)
{
    // Publish と同じ mutex で、結果の有無の確認と通知先の設定をまとめる。
    // 出ている結果は Publish が通知していないので、ここで送っても二重には届かない。
    const std::lock_guard lock(mutex_);
    notify_hwnd_ = hwnd;
    notify_msg_ = msg_id;
    if (outcome_) {
        ::PostMessageW(hwnd, msg_id, 0, 0);
    }
    return in_flight_;
}

void AsyncLoadCoordinator::RunWorker(const std::wstring& path, const Theme& theme, float content_width,
                                     const std::stop_token& stop_token, ReloadBase reload_base)
{
    MENDO_PROFILE("AsyncLoadCoordinator::Start Posted Task");
    // 各段の前の stop check は重い処理を skip するための short-circuit。
    if (stop_token.stop_requested()) {
        return;
    }

    auto file = load_file_(path, stop_token);
    if (!file) {
        Publish(stop_token, std::unexpected(file.error()));
        return;
    }
    if (stop_token.stop_requested()) {
        return;
    }

    std::optional<ReloadCheck> reload;
    if (reload_base.data) {
        // 無変更の保存や書き込み途中でも全文パース (100MB で ~0.6s) と
        // 新旧 Document の二重保持が起きないよう、パース前に判定する。
        // 書き込み途中なら差分も取らずに返し、読み直させる。
        if (IsFileLargerThan(path.c_str(), file->byte_size)) {
            Publish(stop_token, AsyncLoadResult{ .reload = ReloadCheck{ .base = reload_base.data, .still_growing = true } });
            return;
        }
        reload.emplace(ReloadCheck{ AnalyzeReloadDiff(reload_base.data->raw, file->text, reload_base.accept_prefix_shrink), reload_base.data });
        // 以降のパース中に UI が文書を差し替えても旧文書を延命しない。
        reload_base.data.reset();
        if (reload->decision.op != ReloadOp::Replace) {
            Publish(stop_token, AsyncLoadResult{ .reload = std::move(reload) });
            return;
        }
    }

    Document doc = Document::FromNormalizedMarkdown(std::move(file->text), path, stop_token);
    if (stop_token.stop_requested()) {
        return;
    }
    if (reload) {
        // 100MB で約 20ms。
        if (const auto old = reload->base.lock()) {
            reload->ends = FindUnchangedEnds(old->parsed.nodes, doc.GetNodes());
        }
    }

    LayoutCache cache = layout::MakeEstimatedLayoutCache(doc.GetNodes(), theme, content_width, stop_token);
    if (stop_token.stop_requested()) {
        return;
    }

    FileWatchTarget watch;
    if (reload) {
        // パースの間に伸びたのも捕まえるよう、結果を出す直前に確かめる。
        reload->still_growing = IsFileLargerThan(path.c_str(), file->byte_size);
    }
    else if (!stop_token.stop_requested()) {
        watch = OpenWatchTarget(path);
    }
    Publish(stop_token, AsyncLoadResult{ std::move(doc), std::move(cache), std::move(reload), std::move(watch) });
}

void AsyncLoadCoordinator::Publish(const std::stop_token& stop_token, AsyncLoadOutcome outcome)
{
    HWND hwnd = nullptr;
    UINT msg_id = 0;
    {
        const std::lock_guard lock(mutex_);
        if (stop_token.stop_requested()) {
            return;
        }
        outcome_.emplace(std::move(outcome));
        hwnd = notify_hwnd_;
        msg_id = notify_msg_;
    }
    if (hwnd) {
        ::PostMessageW(hwnd, msg_id, 0, 0);
    }
}

std::optional<AsyncLoadOutcome> AsyncLoadCoordinator::Take() noexcept
{
    const std::lock_guard lock(mutex_);
    if (!outcome_) {
        return std::nullopt;
    }
    // 取り出し成功 = 非同期ロードの完結。同期ロードなど別経路から in_flight を触ると
    // race になるため、coordinator の責務に閉じる。
    in_flight_ = false;
    return std::exchange(outcome_, std::nullopt);
}
