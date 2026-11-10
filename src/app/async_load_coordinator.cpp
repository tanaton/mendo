#include "async_load_coordinator.h"
#include "app_constants.h"
#include "file_io.h"
#include "layout_computer.h"
#include "profiler.h"
#include "task_scheduler.h"
#include "utf8_codec.h"
#include <algorithm>
#include <cassert>
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
    LoadedFileDoc result{ .byte_size = r.size };
    if (r.size == 0) {
        return result;
    }

    // 先頭 3 バイトを覗いて UTF-8 BOM (EF BB BF) ならスキップ、なければ先頭へ戻す。
    // 一括読み + erase(0,3) と違い BOM ありファイルでも全体スケールの memmove を発生させない。
    char prefix[3];
    const size_t prefix_read = std::min<size_t>(r.size, sizeof(prefix));
    if (!ReadExact(r.handle.get(), prefix, prefix_read)) {
        return std::unexpected(FileLoadError::ReadFailed);
    }

    const bool has_bom = std::string_view{ prefix, prefix_read } == utf8_codec::kBom;
    if (!has_bom && !SetFilePointerEx(r.handle.get(), LARGE_INTEGER{}, nullptr, FILE_BEGIN)) {
        return std::unexpected(FileLoadError::ReadFailed);
    }
    const size_t out_size = has_bom ? r.size - prefix_read : r.size;

    try {
        result.text.resize_and_overwrite(out_size, [&](char* buf, size_t /*cap*/) noexcept -> size_t {
            return ReadExact(r.handle.get(), buf, out_size) ? out_size : 0;
        });
    } catch (const std::bad_alloc&) {
        return std::unexpected(FileLoadError::TooLarge);
    }
    if (result.text.size() != out_size) {
        return std::unexpected(FileLoadError::ReadFailed);
    }
    NormalizeSource(result.text);
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
    if (stale && *stale) {
        const size_t nodes = (*stale)->doc.GetNodes().size();
        DisposeOnWorkerIfLarge(scheduler_, nodes, kDisposeMinNodes, std::move(stale));
    }
}

void AsyncLoadCoordinator::Start(std::wstring path, Theme theme, float content_width, ReloadBase reload_base)
{
    // 旧 worker の stop を先に要求してから sink を空にし、旧 worker の publish を確実に弾く。
    // request_stop 後の source は再使用できないので作り直す。
    Cancel();
    stop_source_ = std::stop_source{};
    in_flight_ = true;

    // 画像のデコードや図の読み込みが溜まっていても待たせない。リロードは旧文書の表示を続けるので、それらの待ちを捨てられない。
    scheduler_.PostFront([this, path = std::move(path), theme = std::move(theme), content_width, stop_token = stop_source_.get_token(), reload_base = std::move(reload_base), guard = latch_.Acquire()]() mutable {
        try {
            RunWorker(path, theme, content_width, stop_token, std::move(reload_base));
        } catch (const std::bad_alloc&) {
            // 空の表の行補完などは入力の百倍以上を確保させられる。worker で投げると terminate するので失敗として返す。
            Publish(stop_token, std::unexpected(FileLoadError::TooLarge));
        }
    });
}

bool AsyncLoadCoordinator::Attach(HWND hwnd, UINT msg_id)
{
    // Publish と同じ mutex で、結果の有無の確認と通知先の設定をまとめる。
    // 出ている結果は Publish が通知していないので、ここで送っても二重には届かない。
    const std::lock_guard lock(mutex_);
    notify_hwnd_ = hwnd;
    notify_msg_ = msg_id;
    if (outcome_) {
        NotifyLocked();
    }
    return in_flight_;
}

void AsyncLoadCoordinator::RunWorker(const std::wstring& path, const Theme& theme, float content_width,
                                     const std::stop_token& stop_token, ReloadBase reload_base)
{
    MENDO_PROFILE("AsyncLoadCoordinator::RunWorker");
    // 各段の前の stop check は重い処理を skip するための short-circuit。
    if (stop_token.stop_requested()) {
        return;
    }

    // 読む前に監視を始め、読んでから UI が監視を引き継ぐまでの保存も通知で届くようにする。リロードは今の監視を続ける。
    FileWatchTarget watch = reload_base.data ? FileWatchTarget{} : OpenWatchTarget(path);
    // 差分を取らずに旧文書の表示を続けさせ、読み直させる。
    const auto publish_retry = [&](ReloadOp op) {
        Publish(stop_token, AsyncLoadResult{
                                .reload = ReloadCheck{ .decision = { op, std::string_view::npos }, .base = reload_base.data }
        });
    };
    auto file = load_file_(path, stop_token);
    if (!file) {
        if (reload_base.data && file.error() == FileLoadError::Locked) {
            publish_retry(ReloadOp::Locked);
            return;
        }
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
            publish_retry(ReloadOp::StillGrowing);
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

    // パースの間に伸びたのも捕まえるよう、結果を出す直前に確かめる。
    if (reload && IsFileLargerThan(path.c_str(), file->byte_size)) {
        reload->decision.op = ReloadOp::StillGrowing;
    }
    Publish(stop_token, AsyncLoadResult{ std::move(doc), std::move(cache), std::move(reload), std::move(watch) });
}

void AsyncLoadCoordinator::Publish(const std::stop_token& stop_token, AsyncLoadOutcome outcome)
{
    const std::lock_guard lock(mutex_);
    if (stop_token.stop_requested()) {
        return;
    }
    outcome_.emplace(std::move(outcome));
    NotifyLocked();
}

void AsyncLoadCoordinator::NotifyLocked() const noexcept
{
    if (notify_hwnd_) {
        ::PostMessageW(notify_hwnd_, notify_msg_, 0, 0);
    }
}

std::optional<AsyncLoadOutcome> AsyncLoadCoordinator::Take() noexcept
{
    const std::lock_guard lock(mutex_);
    if (!outcome_) {
        return std::nullopt;
    }
    // 取り出し成功 = 非同期ロードの完結。in_flight を倒すのは coordinator の責務に閉じる。
    in_flight_ = false;
    return std::exchange(outcome_, std::nullopt);
}

namespace {

// 待っている間に掛けるタイマーの間隔。待っていなければ 0。
UINT WaitInterval(const ReloadScheduler::State& state) noexcept
{
    const auto* w = std::get_if<ReloadScheduler::Waiting>(&state);
    if (!w) {
        return 0;
    }
    switch (w->wait) {
    case ReloadScheduler::Wait::Debounce:
        return app_timer::FILE_RELOAD_DEBOUNCE_MS;
    case ReloadScheduler::Wait::Retry:
        return app_timer::FILE_RELOAD_RETRY_MS;
    case ReloadScheduler::Wait::Locked:
        return app_timer::FILE_RELOAD_LOCKED_RETRY_MS;
    }
    std::unreachable();
}

} // namespace

ReloadCommand ReloadScheduler::Enter(State next)
{
    const UINT before = WaitInterval(state_);
    state_ = std::move(next);
    const UINT after = WaitInterval(state_);
    if (after == before) {
        return {};
    }
    if (after == 0) {
        return { .kill_timer = true };
    }
    return { .set_timer_ms = after };
}

ReloadCommand ReloadScheduler::StartReload(Attempts attempts)
{
    ReloadCommand cmd = Enter(Reloading{ attempts });
    cmd.start_reload = true;
    return cmd;
}

ReloadCommand ReloadScheduler::OnFileChanged()
{
    if (std::holds_alternative<Idle>(state_)) {
        return Enter(Waiting{});
    }
    // 待っている間の変更は、待ち終えて読む内容に含まれる。
    if (auto* r = std::get_if<Reloading>(&state_)) {
        r->changed = true;
    }
    else if (auto* o = std::get_if<LoadingOther>(&state_)) {
        o->changed = true;
    }
    return {};
}

ReloadCommand ReloadScheduler::OnTimer(bool selecting)
{
    const auto* w = std::get_if<Waiting>(&state_);
    if (!w) {
        return { .kill_timer = true };
    }
    if (selecting) {
        return Enter(Waiting{ Wait::Retry, w->attempts });
    }
    return StartReload(w->attempts);
}

ReloadCommand ReloadScheduler::OnReloadRequested(bool selecting)
{
    if (std::holds_alternative<Reloading>(state_) || std::holds_alternative<LoadingOther>(state_)) {
        return {};
    }
    if (selecting) {
        return Enter(Waiting{ Wait::Retry });
    }
    return StartReload({});
}

ReloadCommand ReloadScheduler::OnLoadStarted(bool same_file)
{
    // 進行中の読み込みは置き換わる。
    if (same_file) {
        return Enter(Reloading{});
    }
    // 待っていた読み直しと、取り消される読み直しは、差し替えずに終わったときに読み直す。
    bool changed = true;
    if (std::holds_alternative<Idle>(state_)) {
        changed = false;
    }
    else if (const auto* o = std::get_if<LoadingOther>(&state_)) {
        changed = o->changed;
    }
    return Enter(LoadingOther{ changed });
}

ReloadCommand ReloadScheduler::OnLoadFinished(ParseCompleteStep step, bool selecting)
{
    if (const auto* o = std::get_if<LoadingOther>(&state_)) {
        return step == ParseCompleteStep::ReplaceWithResult ? Enter(Idle{}) : Settle(o->changed);
    }
    const auto* r = std::get_if<Reloading>(&state_);
    // 読み込みは必ず OnLoadStarted か start_reload で始まるので、ほかの段階で結果は来ない。
    assert(r);
    if (!r) {
        return {};
    }
    Attempts attempts = r->attempts;
    switch (step) {
    case ParseCompleteStep::IgnoreStale:
        return {};
    case ParseCompleteStep::Fail:
    case ParseCompleteStep::ReloadUnchanged:
        return Settle(r->changed);
    case ParseCompleteStep::ReplaceWithResult:
        return Enter(Idle{});
    case ParseCompleteStep::Retry:
        attempts.locked_retries = 0;
        return Enter(Waiting{ Wait::Retry, attempts });
    case ParseCompleteStep::RetryLocked: {
        ++attempts.locked_retries;
        ReloadCommand cmd = Enter(Waiting{ attempts.locked_retries < kLockedRetriesBeforeNotice ? Wait::Retry : Wait::Locked, attempts });
        cmd.notify_locked = attempts.locked_retries == kLockedRetriesBeforeNotice;
        return cmd;
    }
    case ParseCompleteStep::DeferPrefixShrink:
        // 正確な差分を取り直す。上限に達したら、次の読み直しで worker が縮んだ内容を受け入れる (AcceptPrefixShrink)。
        ++attempts.prefix_shrink_defers;
        attempts.locked_retries = 0;
        return Enter(Waiting{ Wait::Retry, attempts });
    case ParseCompleteStep::ApplyReload: {
        // 読み始めた後にテキスト選択のドラッグが始まった。読み直しはドラッグの後に回す。
        if (selecting) {
            return Enter(Waiting{ Wait::Retry });
        }
        ReloadCommand cmd = Settle(r->changed);
        cmd.apply_result = true;
        return cmd;
    }
    }
    std::unreachable();
}

ReloadCommand ReloadScheduler::OnOtherLoadCancelled()
{
    if (const auto* o = std::get_if<LoadingOther>(&state_)) {
        return Settle(o->changed);
    }
    return {};
}
