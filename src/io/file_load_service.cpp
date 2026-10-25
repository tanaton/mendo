#include "file_load_service.h"
#include "theme.h"

void FileLoadService::StartAsyncLoad(TaskScheduler& scheduler, std::pmr::wstring path, HWND hwnd, UINT msg_id, Theme theme,
                                     float content_width, ReloadBase reload_base)
{
    loading_path_ = path;
    // 進行中の読み込み (preload を含む) は Start が世代を進めて置き換える。
    coordinator_.Start(scheduler, std::move(path), hwnd, msg_id, std::move(theme), content_width, std::move(reload_base));
}

void FileLoadService::StartPreloadAsync(TaskScheduler& scheduler, std::pmr::wstring path, Theme theme)
{
    loading_path_ = path;
    coordinator_.Start(scheduler, std::move(path), nullptr, 0, std::move(theme));
}
