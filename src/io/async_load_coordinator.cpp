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

void AsyncLoadCoordinator::Start(TaskScheduler& scheduler, std::pmr::wstring path, HWND hwnd, UINT msg_id, const Theme& theme)
{
    ResetSinks();
    in_flight_ = true;
    // 前 worker のキャプチャ済み stop_token を協調キャンセルする。これを呼ばないと
    // 古い source は stop_requested=false のまま残り続ける。
    stop_source_.request_stop();
    // request_stop 済みの source は再使用不可なので Start 毎に作り直す。
    stop_source_ = std::stop_source{};
    auto stop_token = stop_source_.get_token();
    const uint32_t gen = gen_.fetch_add(1, std::memory_order_relaxed) + 1;

    const bool posted = scheduler.Post([this, path = std::move(path), hwnd, msg_id, gen, theme, stop_token = std::move(stop_token), guard = latch_.Acquire()] {
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

        if (gen_.load(std::memory_order_relaxed) != gen) {
            return;
        }

        auto load_result = FileLoader::LoadFile(path);
        if (!load_result) {
            try_publish([&] { error_ = load_result.error(); });
            return;
        }

        if (gen_.load(std::memory_order_relaxed) != gen || stop_token.stop_requested()) {
            return;
        }

        Document doc = Document::FromMarkdown(std::move(load_result->text), load_result->byte_size, path, stop_token);

        if (gen_.load(std::memory_order_relaxed) != gen || stop_token.stop_requested()) {
            return;
        }

        LayoutCache cache;
        cache.Reset(doc.GetNodes().size(), /* shrink = */ false);
        EstimateNodeHeights(doc.GetNodes(), cache, theme, stop_token);

        if (stop_token.stop_requested()) {
            return;
        }

        try_publish([&] {
            result_.emplace(AsyncLoadResult{ std::move(doc), std::move(cache), /* heights_estimated = */ true });
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
