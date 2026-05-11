#include "file_load_service.h"
#include "file_loader.h"
#include "layout.h"
#include "profiler.h"

void FileLoadService::StartLoading(std::pmr::wstring path)
{
    loading_path_ = std::move(path);
    animation_.Begin();
}

void FileLoadService::StopLoading() noexcept
{
    animation_.End();
}

std::expected<void, FileLoadError> FileLoadService::ExecuteLoad(Document& doc, LayoutCache& cache)
{
    StopLoading();

    auto result = DocumentService::LoadFile(loading_path_);
    if (!result) {
        return std::unexpected(result.error());
    }
    doc = std::move(*result);
    cache.Reset(doc.GetNodes().size());
    return {};
}

void FileLoadService::StartAsyncLoad(TaskScheduler& scheduler, HWND hwnd, UINT msg_id, const Theme& theme)
{
    coordinator_.Start(scheduler, loading_path_, hwnd, msg_id, theme);
}

std::optional<AsyncLoadResult> FileLoadService::TakeAsyncResult()
{
    // preload と StartAsyncLoad は同時進行しない (preload 完了後に通常 load フローへ合流)。
    if (auto r = preloader_.TakeResult()) {
        return r;
    }
    return coordinator_.TakeResult();
}

std::optional<FileLoadError> FileLoadService::TakeAsyncError() noexcept
{
    if (auto e = preloader_.TakeError()) {
        return e;
    }
    return coordinator_.TakeError();
}

void FileLoadService::StartPreloadAsync(std::pmr::wstring path)
{
    loading_path_ = path;
    preloader_.Start(std::move(path));
}
