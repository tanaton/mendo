#include "file_load_service.h"
#include "document_service.h"
#include "theme.h"

void FileLoadService::StartLoading(std::pmr::wstring path)
{
    loading_path_ = std::move(path);
    animation_.Begin();
}

void FileLoadService::StopLoading() noexcept
{
    animation_.End();
}

std::expected<Document, FileLoadError> FileLoadService::ExecuteLoad()
{
    StopLoading();
    return DocumentService::LoadFile(loading_path_);
}

void FileLoadService::StartAsyncLoad(TaskScheduler& scheduler, HWND hwnd, UINT msg_id, const Theme& theme,
                                     std::shared_ptr<const std::pmr::string> reload_base)
{
    // 進行中の読み込み (preload を含む) は Start が世代を進めて置き換える。
    coordinator_.Start(scheduler, loading_path_, hwnd, msg_id, theme, std::move(reload_base));
}

void FileLoadService::StartPreloadAsync(TaskScheduler& scheduler, std::pmr::wstring path, const Theme& theme)
{
    loading_path_ = path;
    coordinator_.Start(scheduler, std::move(path), nullptr, 0, theme);
}
