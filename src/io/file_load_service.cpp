#include "file_load_service.h"
#include "file_loader.h"
#include "layout.h"
#include "profiler.h"

void FileLoadService::StartLoading(std::pmr::wstring path)
{
    loading_path_ = std::move(path);
    animation_.Begin();
    async_in_flight_ = true;
}

void FileLoadService::StopLoading() noexcept
{
    animation_.End();
    async_in_flight_ = false;
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

void FileLoadService::ResetAsyncState()
{
    const std::lock_guard lock(async_mutex_);
    async_result_.reset();
    async_error_.reset();
}

void FileLoadService::StartAsyncLoad(TaskScheduler& scheduler, HWND hwnd, UINT msg_id, const Theme& theme)
{
    ResetAsyncState();
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
                {
                    const std::lock_guard lock(async_mutex_);
                    async_error_ = load_result.error();
                }
                ::PostMessageW(hwnd, msg_id, 0, 0);
            }
            return;
        }

        if (async_gen_.load(std::memory_order_relaxed) != gen) {
            return;
        }

        Document doc = Document::FromMarkdown(std::move(load_result->text), load_result->byte_size, path);

        if (async_gen_.load(std::memory_order_relaxed) != gen) {
            return;
        }

        LayoutCache cache;
        cache.Reset(doc.GetNodes().size(), /* shrink = */ false);
        EstimateNodeHeights(doc.GetNodes(), cache, theme);

        {
            const std::lock_guard lock(async_mutex_);
            async_result_.emplace(AsyncLoadResult{ std::move(doc), std::move(cache), /* heights_estimated = */ true });
        }

        ::PostMessageW(hwnd, msg_id, 0, 0);
    });
}

std::optional<AsyncLoadResult> FileLoadService::TakeAsyncResult()
{
    // preload と StartAsyncLoad は同時進行しない (preload 完了後に通常 load フローへ合流)。
    if (auto r = preloader_.TakeResult()) {
        return r;
    }
    const std::lock_guard lock(async_mutex_);
    auto result = std::move(async_result_);
    async_result_.reset();
    return result;
}

std::optional<FileLoadError> FileLoadService::TakeAsyncError() noexcept
{
    if (auto e = preloader_.TakeError()) {
        return e;
    }
    const std::lock_guard lock(async_mutex_);
    auto err = async_error_;
    async_error_.reset();
    return err;
}

void FileLoadService::StartPreloadAsync(std::pmr::wstring path)
{
    // async_in_flight_ は触らない: preload 中の状態管理は preloader_ に閉じる
    // (IsAsyncLoading() が両者を OR で見る)。
    loading_path_ = path;
    preloader_.Start(std::move(path));
}
