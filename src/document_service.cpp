#include "document_service.h"
#include "parser.h"

bool DocumentService::LoadFile(const std::wstring& path, Document& doc) {
    std::string content = FileLoader::LoadFile(path);
    if (content.empty() && GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES) {
        return false;
    }
    doc = Document::FromMarkdown(content, path);
    return true;
}

bool DocumentService::ReloadFile(Document& doc) {
    if (doc.GetFilePath().empty()) return false;
    std::string content = FileLoader::LoadFile(doc.GetFilePath());
    doc.ReplaceContent(ParseMarkdown(content));
    return true;
}

void DocumentService::StartWatching(const std::wstring& path, FileLoader::ChangeCallback cb) {
    loader_.StartWatching(path, std::move(cb));
}

void DocumentService::StopWatching() {
    loader_.StopWatching();
}

bool DocumentService::NeedsLoadingAnimation(const std::wstring& path) {
    static constexpr DWORD LOADING_ANIM_THRESHOLD = 128 * 1024;
    WIN32_FILE_ATTRIBUTE_DATA attr{};
    if (GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &attr)
        && attr.nFileSizeHigh == 0 && attr.nFileSizeLow <= LOADING_ANIM_THRESHOLD) {
        return false;
    }
    return true;
}
