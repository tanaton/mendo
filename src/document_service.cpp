#include "document_service.h"

bool DocumentService::LoadFile(std::wstring_view path, Document& doc) {
    std::pmr::wstring path_str{ path };
    std::pmr::string content = FileLoader::LoadFile(path_str);
    if (content.empty() && GetFileAttributesW(path_str.c_str()) == INVALID_FILE_ATTRIBUTES) {
        return false;
    }
    doc = Document::FromMarkdown(content, path);
    return true;
}

bool DocumentService::ReloadFile(Document& doc) {
    if (doc.GetFilePath().empty()) {
        return false;
    }
    doc.ReplaceFromMarkdown(FileLoader::LoadFile(doc.GetFilePath()));
    return true;
}

void DocumentService::StartWatching(std::wstring_view path, FileLoader::ChangeCallback cb) {
    loader_.StartWatching(std::pmr::wstring{ path }, std::move(cb));
}

void DocumentService::StopWatching() noexcept {
    loader_.StopWatching();
}

void DocumentService::CheckForChanges() {
    loader_.CheckForChanges();
}

bool DocumentService::NeedsLoadingAnimation(std::wstring_view path) noexcept {
    static constexpr DWORD LOADING_ANIM_THRESHOLD = 128 * 1024;
    WIN32_FILE_ATTRIBUTE_DATA attr{};
    std::pmr::wstring path_str{ path };
    if (GetFileAttributesExW(path_str.c_str(), GetFileExInfoStandard, &attr)
        && attr.nFileSizeHigh == 0 && attr.nFileSizeLow <= LOADING_ANIM_THRESHOLD) {
        return false;
    }
    return true;
}
