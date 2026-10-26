#include "async_load_coordinator.h"
#include "file_io.h"
#include "layout_computer.h"
#include "profiler.h"
#include "task_scheduler.h"
#include <new>
#include <utility>

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
}

void AsyncLoadCoordinator::Start(TaskScheduler& scheduler, std::pmr::wstring path, HWND hwnd, UINT msg_id, Theme theme, float content_width,
                                 ReloadBase reload_base)
{
    // Cancel() と同じく gen を最初に進めて、旧 worker の publish を確実に弾く。
    const uint32_t gen = gen_.fetch_add(1, std::memory_order_relaxed) + 1;
    ResetSinks();
    {
        const std::lock_guard lock(mutex_);
        notify_hwnd_ = hwnd;
        notify_msg_ = msg_id;
    }
    in_flight_ = true;
    // 前 worker がキャプチャ済みの stop_token を協調キャンセルしてから、再使用不可の source を作り直す。
    stop_source_.request_stop();
    stop_source_ = std::stop_source{};

    // 画像のデコードや図の読み込みが溜まっていても待たせない。リロードは旧文書の表示を続けるので、それらの待ちを捨てられない。
    const bool posted = scheduler.PostFront([this, &scheduler, path = std::move(path), gen, theme = std::move(theme), content_width, stop_token = stop_source_.get_token(), reload_base = std::move(reload_base), guard = latch_.Acquire()]() mutable {
        try {
            RunWorker(scheduler, path, gen, theme, content_width, stop_token, std::move(reload_base));
        } catch (const std::bad_alloc&) {
            // 空の表の行補完などは入力の百倍以上を確保させられる。worker で投げると terminate するので失敗として返す。
            Publish(gen, std::unexpected(FileLoadError::TooLarge));
        }
    });

    if (!posted) {
        // Shutdown 後。lambda が走らないので latch::Guard は capture 内で
        // destruct され自動的に Release される。in_flight_/outcome_ を整えて UI に通知することで
        // 以降のリロードを再開可能にする。
        {
            const std::lock_guard lock(mutex_);
            outcome_.emplace(std::unexpected(FileLoadError::ReadFailed));
            in_flight_ = false;
        }
        if (hwnd) {
            ::PostMessageW(hwnd, msg_id, 0, 0);
        }
    }
}

AsyncLoadCoordinator::AttachResult AsyncLoadCoordinator::Attach(HWND hwnd, UINT msg_id)
{
    if (!in_flight_) {
        return AttachResult::None;
    }
    // Publish と同じ mutex で、結果の有無の確認と通知先の設定をまとめる。
    const std::lock_guard lock(mutex_);
    if (outcome_) {
        return AttachResult::AppliedSync;
    }
    notify_hwnd_ = hwnd;
    notify_msg_ = msg_id;
    return AttachResult::AttachedAsync;
}

void AsyncLoadCoordinator::RunWorker(TaskScheduler& scheduler, const std::pmr::wstring& path, uint32_t gen, const Theme& theme, float content_width,
                                     const std::stop_token& stop_token, ReloadBase reload_base)
{
    MENDO_PROFILE("AsyncLoadCoordinator::Start Posted Task");
    // 各段の前の gen/stop check は重い処理を skip するための short-circuit。
    const auto publish = [this, gen](AsyncLoadOutcome outcome) {
        Publish(gen, std::move(outcome));
    };

    if (IsStale(gen) || stop_token.stop_requested()) {
        return;
    }

    auto file = load_file_(path, stop_token);
    if (!file) {
        publish(std::unexpected(file.error()));
        return;
    }
    if (stop_token.stop_requested()) {
        return;
    }

    std::optional<ReloadCheck> reload;
    if (reload_base.text) {
        // 無変更の保存や書き込み途中でも全文パース (100MB で ~0.6s) と
        // 新旧 Document の二重保持が起きないよう、パース前に判定する。
        const ReloadDecision decision = DecideReload(IsFileLargerThan(path.c_str(), file->byte_size), *reload_base.text, file->text);
        reload.emplace(ReloadCheck{ decision, reload_base.text, path, file->byte_size });
        // 以降のパース中に UI が文書を差し替えても旧テキストを延命しない。
        reload_base.text.reset();
        const auto op = reload->decision.op;
        if (op != ReloadOp::Replace) {
            publish(AsyncLoadResult{ .reload = std::move(reload) });
            return;
        }
    }

    Document doc = Document::FromMarkdown(std::move(file->text), file->byte_size, path, stop_token, &scheduler);
    if (stop_token.stop_requested() || IsStale(gen)) {
        return;
    }
    if (reload) {
        // 旧ノードのテキストは旧原文を指すので、両方を生かしたまま比べる (100MB で約 20ms)。
        const auto old_text = reload->base.lock();
        if (const auto old_nodes = reload_base.nodes.lock(); old_text && old_nodes) {
            reload->ends = FindUnchangedEnds(old_nodes->nodes, doc.GetNodes());
        }
    }

    LayoutCache cache = layout::MakeEstimatedLayoutCache(doc.GetNodes(), theme, content_width, stop_token);
    if (stop_token.stop_requested()) {
        return;
    }

    publish(AsyncLoadResult{ std::move(doc), std::move(cache), std::move(reload) });
}

void AsyncLoadCoordinator::Publish(uint32_t gen, AsyncLoadOutcome outcome)
{
    HWND hwnd = nullptr;
    UINT msg_id = 0;
    {
        const std::lock_guard lock(mutex_);
        if (IsStale(gen)) {
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
