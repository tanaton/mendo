#include "document_service.h"
#include "file_loader.h"
#include "profiler.h"

std::expected<Document, FileLoadError> DocumentService::LoadFile(const std::pmr::wstring& path)
{
    auto result = FileLoader::LoadFile(path);
    if (!result) {
        return std::unexpected(result.error());
    }
    MENDO_PROFILE("Document::FromMarkdown");
    return Document::FromMarkdown(std::move(result->text), result->byte_size, path);
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

static bool ExceedsFileSize(const std::pmr::wstring& path, DWORD threshold) noexcept
{
    WIN32_FILE_ATTRIBUTE_DATA attr{};
    if (GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &attr) && attr.nFileSizeHigh == 0 && attr.nFileSizeLow <= threshold) {
        return false;
    }
    return true;
}

bool DocumentService::NeedsAsyncLoad(const std::pmr::wstring& path) noexcept
{
    static constexpr DWORD ASYNC_LOAD_THRESHOLD = 64 * 1024;
    return ExceedsFileSize(path, ASYNC_LOAD_THRESHOLD);
}

bool DocumentService::NeedsLoadingAnimation(const std::pmr::wstring& path) noexcept
{
    static constexpr DWORD LOADING_ANIM_THRESHOLD = 16 * 1024 * 1024;
    return ExceedsFileSize(path, LOADING_ANIM_THRESHOLD);
}
