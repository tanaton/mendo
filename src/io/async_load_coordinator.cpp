#include "async_load_coordinator.h"
#include "document.h"
#include "layout.h"
#include "layout_cache.h"

void AsyncLoadCoordinator::ResetSinks()
{
    const std::lock_guard lock(mutex_);
    result_.reset();
    error_.reset();
}

void AsyncLoadCoordinator::Start(TaskScheduler& scheduler, std::pmr::wstring path, HWND hwnd, UINT msg_id, const Theme& theme)
{
    ResetSinks();
    in_flight_ = true;
    const uint32_t gen = gen_.fetch_add(1, std::memory_order_relaxed) + 1;

    scheduler.Post([this, path = std::move(path), hwnd, msg_id, gen, theme] {
        if (gen_.load(std::memory_order_relaxed) != gen) {
            return;
        }

        auto load_result = FileLoader::LoadFile(path);
        if (!load_result) {
            if (gen_.load(std::memory_order_relaxed) == gen) {
                {
                    const std::lock_guard lock(mutex_);
                    error_ = load_result.error();
                }
                ::PostMessageW(hwnd, msg_id, 0, 0);
            }
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

        {
            const std::lock_guard lock(mutex_);
            result_.emplace(AsyncLoadResult{ std::move(doc), std::move(cache), /* heights_estimated = */ true });
        }

        ::PostMessageW(hwnd, msg_id, 0, 0);
    });
}

std::optional<AsyncLoadResult> AsyncLoadCoordinator::TakeResult()
{
    const std::lock_guard lock(mutex_);
    auto result = std::move(result_);
    result_.reset();
    return result;
}

std::optional<FileLoadError> AsyncLoadCoordinator::TakeError() noexcept
{
    const std::lock_guard lock(mutex_);
    auto err = error_;
    error_.reset();
    return err;
}
