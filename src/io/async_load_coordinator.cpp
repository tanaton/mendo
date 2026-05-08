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
        // 重い処理 (I/O / Parse / Estimate) の手前で stale gen を short-circuit する。
        // sink への書き込みは Cancel との直列化のため lock 内で gen を再確認してから行う。
        if (gen_.load(std::memory_order_relaxed) != gen) {
            return;
        }

        auto load_result = FileLoader::LoadFile(path);
        if (!load_result) {
            bool published = false;
            {
                const std::lock_guard lock(mutex_);
                if (gen_.load(std::memory_order_relaxed) == gen) {
                    error_ = load_result.error();
                    published = true;
                }
            }
            if (published) {
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

        bool published = false;
        {
            const std::lock_guard lock(mutex_);
            if (gen_.load(std::memory_order_relaxed) == gen) {
                result_.emplace(AsyncLoadResult{ std::move(doc), std::move(cache), /* heights_estimated = */ true });
                published = true;
            }
        }
        if (published) {
            ::PostMessageW(hwnd, msg_id, 0, 0);
        }
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
