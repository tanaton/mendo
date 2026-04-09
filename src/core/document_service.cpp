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

bool DocumentService::NeedsLoadingAnimation(const std::pmr::wstring& path) noexcept
{
    static constexpr DWORD LOADING_ANIM_THRESHOLD = 16 * 1024 * 1024;
    WIN32_FILE_ATTRIBUTE_DATA attr{};
    if (GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &attr)
        && attr.nFileSizeHigh == 0 && attr.nFileSizeLow <= LOADING_ANIM_THRESHOLD) {
        return false;
    }
    return true;
}
