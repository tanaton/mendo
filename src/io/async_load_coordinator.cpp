#include "async_load_coordinator.h"
#include "document.h"
#include "layout.h"
#include "layout_cache.h"

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
    const uint32_t gen = gen_.fetch_add(1, std::memory_order_relaxed) + 1;

    scheduler.Post([this, path = std::move(path), hwnd, msg_id, gen, theme] {
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

        if (gen_.load(std::memory_order_relaxed) != gen) {
            return;
        }

        Document doc = Document::FromMarkdown(std::move(load_result->text), load_result->byte_size, path);

        if (gen_.load(std::memory_order_relaxed) != gen) {
            return;
        }

        LayoutCache cache;
        cache.Reset(doc.GetNodes().size(), /* shrink = */ false);
        EstimateNodeHeights(doc.GetNodes(), cache, theme);

        try_publish([&] {
            result_.emplace(AsyncLoadResult{ std::move(doc), std::move(cache), /* heights_estimated = */ true });
        });
    });
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
