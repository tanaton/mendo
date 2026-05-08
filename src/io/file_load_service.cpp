#include "file_load_service.h"
#include "file_loader.h"
#include "layout.h"
#include "profiler.h"

void FileLoadService::StartLoading(std::pmr::wstring path)
{
    loading_path_ = std::move(path);
    animation_.Begin();
    // coordinator_ の in_flight は StartAsyncLoad 経由で立てる。同期 ExecuteLoad 経路では
    // animation_ と loading_path_ のみで進行を表現する。
}

void FileLoadService::StopLoading() noexcept
{
    // animation のみ停止。coordinator の in_flight 管理は coordinator 自身に閉じる
    // (Cancel / TakeResult / TakeError で完結) ので同期 ExecuteLoad 経路から触らない。
    animation_.End();
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
    // coordinator_ の in_flight は触らない: preload 中の状態管理は preloader_ に閉じる
    // (IsAsyncLoading() が両者を OR で見る)。
    loading_path_ = path;
    preloader_.Start(std::move(path));
}
