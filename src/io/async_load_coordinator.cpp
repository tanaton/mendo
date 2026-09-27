#include "async_load_coordinator.h"
#include "document.h"
#include "layout.h"
#include "layout_cache.h"
#include "profiler.h"

AsyncLoadCoordinator::~AsyncLoadCoordinator()
{
    Cancel();
    latch_.Wait();
}

void AsyncLoadCoordinator::ResetSinks() noexcept
{
    std::optional<AsyncLoadResult> stale_result;
    std::optional<FileLoadError> stale_error;
    {
        const std::lock_guard lock(mutex_);
        stale_result.swap(result_);
        stale_error.swap(error_);
    }
    // stale_result / stale_error はここで lock 外で破棄される。
}

void AsyncLoadCoordinator::Start(TaskScheduler& scheduler, std::pmr::wstring path, HWND hwnd, UINT msg_id, const Theme& theme,
                                 std::shared_ptr<const std::pmr::string> reload_base)
{
    // Cancel() と同じく gen を最初に進めて、旧 worker の try_publish を確実に弾く。
    const uint32_t gen = gen_.fetch_add(1, std::memory_order_relaxed) + 1;
    ResetSinks();
    in_flight_ = true;
    // 前 worker のキャプチャ済み stop_token を協調キャンセルする。これを呼ばないと
    // 古い source は stop_requested=false のまま残り続ける。
    stop_source_.request_stop();
    // request_stop 済みの source は再使用不可なので Start 毎に作り直す。
    stop_source_ = std::stop_source{};
    auto stop_token = stop_source_.get_token();

    const bool posted = scheduler.Post([this, path = std::move(path), hwnd, msg_id, gen, theme, stop_token = std::move(stop_token), reload_base = std::move(reload_base), guard = latch_.Acquire()]() mutable {
        MENDO_PROFILE("AsyncLoadCoordinator::Start Posted Task");
        // sink 書き込みは Cancel との直列化のため lock 内で gen を再確認してから行う。
        // I/O / Parse / Estimate の前段の gen check は重い処理を skip するための short-circuit。
        auto try_publish = [this, hwnd, msg_id, gen](auto&& assign_sink) {
            bool published = false;
            {
                const std::lock_guard lock(mutex_);
                if (gen_.load(std::memory_order_relaxed) == gen) {
                    assign_sink();
                    published = true;
                }
            }
            if (published) {
                ::PostMessageW(hwnd, msg_id, 0, 0);
            }
        };

        if (gen_.load(std::memory_order_relaxed) != gen || stop_token.stop_requested()) {
            return;
        }

        auto file = FileLoader::LoadFile(path);
        if (!file) {
            try_publish([&] { error_ = file.error(); });
            return;
        }
        if (stop_token.stop_requested()) {
            return;
        }

        std::optional<ReloadCheck> reload;
        if (reload_base) {
            // 無変更の保存や truncate→rewrite の前半でも全文パース (100MB で ~0.6s) と
            // 新旧 Document の二重保持が起きないよう、パース前に差分判定する。
            NormalizeNewlines(file->text);
            reload.emplace(ReloadCheck{ AnalyzeReloadDiff(*reload_base, file->text), reload_base, path, file->byte_size });
            // 以降のパース中に UI が文書を差し替えても旧テキストを延命しない。
            reload_base.reset();
            const auto op = reload->decision.op;
            if (op == ReloadOp::NoChange || op == ReloadOp::DeferPrefixShrink) {
                try_publish([&] {
                    result_.emplace(AsyncLoadResult{ .reload = std::move(reload) });
                });
                return;
            }
        }

        Document doc;
        {
            MENDO_PROFILE("Document::FromMarkdown");
            doc = Document::FromMarkdown(std::move(file->text), file->byte_size, path, stop_token);
        }
        if (stop_token.stop_requested() || gen_.load(std::memory_order_relaxed) != gen) {
            return;
        }

        LayoutCache cache;
        cache.Reset(doc.GetNodes().size(), /* shrink = */ false);
        EstimateNodeHeights(doc.GetNodes(), cache, theme, stop_token);

        if (stop_token.stop_requested()) {
            return;
        }

        try_publish([&] {
            result_.emplace(AsyncLoadResult{ std::move(doc), std::move(cache), /* heights_estimated = */ true, std::move(reload) });
        });
    });

    if (!posted) {
        // queue 飽和 or Shutdown 後。lambda が走らないので latch::Guard は capture 内で
        // destruct され自動的に Release される。in_flight_/error_ を整えて UI に通知することで
        // 以降のリロードを再開可能にする。
        {
            const std::lock_guard lock(mutex_);
            error_ = FileLoadError::ReadFailed;
            in_flight_ = false;
        }
        ::PostMessageW(hwnd, msg_id, 0, 0);
    }
}

std::optional<AsyncLoadResult> AsyncLoadCoordinator::TakeResult()
{
    const std::lock_guard lock(mutex_);
    if (!result_) {
        return std::nullopt;
    }
    auto result = std::move(result_);
    result_.reset();
    // 取り出し成功 = 非同期ロードの完結。これ以降 IsActive() は false を返す。
    // 同期 ExecuteLoad など別経路から in_flight を触ると race になるため、coordinator の責務に閉じる。
    in_flight_ = false;
    return result;
}

std::optional<FileLoadError> AsyncLoadCoordinator::TakeError() noexcept
{
    const std::lock_guard lock(mutex_);
    if (!error_) {
        return std::nullopt;
    }
    auto err = error_;
    error_.reset();
    in_flight_ = false;
    return err;
}
