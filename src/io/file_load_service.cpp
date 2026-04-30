#include "file_load_service.h"
#include "file_loader.h"
#include "layout.h"
#include "profiler.h"
#include "ui_constants.h"

void FileLoadService::StartLoading(std::pmr::wstring path)
{
    loading_path_ = std::move(path);
    loading_ = true;
    async_in_flight_ = true;
    loading_angle_ = 0.0f;
}

void FileLoadService::StopLoading() noexcept
{
    loading_ = false;
    async_in_flight_ = false;
}

void FileLoadService::TickLoadingAnimation() noexcept
{
    loading_angle_ += spinner::ROTATION_INCREMENT;
    if (loading_angle_ > TWO_PI) {
        loading_angle_ -= TWO_PI;
    }
}

std::expected<void, FileLoadError> FileLoadService::ExecuteLoad(Document& doc, LayoutCache& cache)
{
    StopLoading();

    auto result = doc_service_.LoadFile(loading_path_);
    if (!result) {
        return std::unexpected(result.error());
    }
    doc = std::move(*result);
    cache.Reset(doc.GetNodes().size());
    return {};
}

void FileLoadService::StartAsyncLoad(TaskScheduler& scheduler, HWND hwnd, UINT msg_id, const Theme& theme)
{
    {
        const std::lock_guard lock(async_mutex_);
        async_result_.reset();
    }
    async_in_flight_ = true;
    const uint32_t gen = async_gen_.fetch_add(1, std::memory_order_relaxed) + 1;

    // ワーカースレッドは loading_path_ に触らないため、capture コピーで安全。
    // theme も値コピー: UI スレッドの SetTheme は std::wstring メンバ (font_family など) を
    // 非アトミックに書き換えるので、参照キャプチャだと worker の EstimateNodeHeights が
    // 文字列リードと同時にレースする。
    scheduler.Post([this, path = loading_path_, hwnd, msg_id, gen, theme] {
        if (async_gen_.load(std::memory_order_relaxed) != gen) {
            return;
        }

        auto load_result = FileLoader::LoadFile(path);
        if (!load_result) {
            if (async_gen_.load(std::memory_order_relaxed) == gen) {
                PostMessage(hwnd, msg_id, 0, 0);
            }
            return;
        }

        if (async_gen_.load(std::memory_order_relaxed) != gen) {
            return;
        }

        Document doc = Document::FromMarkdown(std::move(load_result->wide), load_result->byte_size, path);

        if (async_gen_.load(std::memory_order_relaxed) != gen) {
            return;
        }

        LayoutCache cache;
        cache.Reset(doc.GetNodes().size(), /* shrink = */ false);
        EstimateNodeHeights(doc.GetNodes(), cache, theme);

        {
            const std::lock_guard lock(async_mutex_);
            async_result_.emplace(AsyncLoadResult{ std::move(doc), std::move(cache) });
        }

        PostMessage(hwnd, msg_id, 0, 0);
    });
}

std::optional<AsyncLoadResult> FileLoadService::TakeAsyncResult()
{
    const std::lock_guard lock(async_mutex_);
    auto result = std::move(async_result_);
    async_result_.reset();
    return result;
}
