#include "document_service.h"
#include "profiler.h"

bool DocumentService::LoadFile(const std::pmr::wstring& path, Document& doc)
{
    std::pmr::string content;
    {
        MENDO_PROFILE("FileLoader::LoadFile");
        content = FileLoader::LoadFile(path);
    }
    if (content.empty() && GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES) {
        return false;
    }
    {
        MENDO_PROFILE("Document::FromMarkdown");
        doc = Document::FromMarkdown(std::move(content), path);
    }
    return true;
}

bool DocumentService::ReloadFile(Document& doc)
{
    if (doc.GetFilePath().empty()) {
        return false;
    }
    doc.ReplaceFromMarkdown(FileLoader::LoadFile(doc.GetFilePath()));
    return true;
}

void DocumentService::StartWatching(const std::pmr::wstring& path, FileLoader::ChangeCallback cb)
{
    loader_.StartWatching(path, std::move(cb));
}

void DocumentService::StopWatching() noexcept
{
    loader_.StopWatching();
}

void DocumentService::CheckForChanges()
{
    loader_.CheckForChanges();
}

void DocumentService::ResetDebounceTick() noexcept
{
    loader_.ResetDebounceTick();
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
