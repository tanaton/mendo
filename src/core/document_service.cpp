#include "document_service.h"
#include "file_loader.h"
#include "profiler.h"

bool DocumentService::LoadFile(const std::pmr::wstring& path, Document& doc)
{
    std::expected<std::pmr::string, FileLoadError> result;
    {
        MENDO_PROFILE("FileLoader::LoadFile");
        result = FileLoader::LoadFile(path);
    }
    if (!result) {
        return false;
    }
    {
        MENDO_PROFILE("Document::FromMarkdown");
        doc = Document::FromMarkdown(std::move(*result), path);
    }
    return true;
}

void DocumentService::StartWatching(const std::pmr::wstring& path, FileWatcher::ChangeCallback cb)
{
    watcher_.StartWatching(path, std::move(cb));
}

void DocumentService::StopWatching() noexcept
{
    watcher_.StopWatching();
}

void DocumentService::CheckForChanges()
{
    watcher_.CheckForChanges();
}

void DocumentService::ResumeWatching()
{
    watcher_.ResumeWatching();
}

// ファイルサイズがしきい値を超えるか判定するヘルパー。
// サイズ取得に失敗した場合（存在しないファイル等）は true を返す。
static bool ExceedsFileSize(const std::pmr::wstring& path, DWORD threshold) noexcept
{
    WIN32_FILE_ATTRIBUTE_DATA attr{};
    if (GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &attr)
        && attr.nFileSizeHigh == 0 && attr.nFileSizeLow <= threshold) {
        return false;
    }
    return true;
}

bool DocumentService::NeedsAsyncLoad(const std::pmr::wstring& path) noexcept
{
    // 64KB 超: パースに数十ms以上かかりUIブロックが体感される
    static constexpr DWORD ASYNC_LOAD_THRESHOLD = 64 * 1024;
    return ExceedsFileSize(path, ASYNC_LOAD_THRESHOLD);
}

bool DocumentService::NeedsLoadingAnimation(const std::pmr::wstring& path) noexcept
{
    // 16MB 超: パースに数秒かかるためスピナーを表示する
    static constexpr DWORD LOADING_ANIM_THRESHOLD = 16 * 1024 * 1024;
    return ExceedsFileSize(path, LOADING_ANIM_THRESHOLD);
}
