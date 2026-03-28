#include "document_service.h"

bool DocumentService::LoadFile(const std::pmr::wstring& path, Document& doc)
{
    std::pmr::string content = FileLoader::LoadFile(path);
    if (content.empty() && GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES) {
        return false;
    }
    doc = Document::FromMarkdown(content, path);
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

bool DocumentService::NeedsLoadingAnimation(const std::pmr::wstring& path) noexcept
{
    static constexpr DWORD LOADING_ANIM_THRESHOLD = 128 * 1024;
    WIN32_FILE_ATTRIBUTE_DATA attr{};
    if (GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &attr)
        && attr.nFileSizeHigh == 0 && attr.nFileSizeLow <= LOADING_ANIM_THRESHOLD) {
        return false;
    }
    return true;
}
