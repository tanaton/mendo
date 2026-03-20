#include "file_load_service.h"

FileLoadService::FileLoadService(DocumentService& doc_service) noexcept
    : doc_service_(doc_service) {}

void FileLoadService::StartLoading(const std::wstring& path) {
    loading_path_ = path;
    loading_ = true;
    loading_angle_ = 0.0f;
}

void FileLoadService::StopLoading() noexcept {
    loading_ = false;
}

void FileLoadService::TickLoadingAnimation() noexcept {
    loading_angle_ += 0.15f;
    if (loading_angle_ > 6.2831853f) loading_angle_ -= 6.2831853f;
}

bool FileLoadService::ExecuteLoad(Document& doc, LayoutCache& cache) {
    StopLoading();

    if (!doc_service_.LoadFile(loading_path_, doc)) {
        return false;
    }
    cache.Reset(doc.GetNodes().size());
    return true;
}

bool FileLoadService::ExecuteReload(Document& doc, LayoutCache& cache) {
    if (doc.GetFilePath().empty()) return false;

    doc_service_.ReloadFile(doc);
    cache.Reset(doc.GetNodes().size());
    return true;
}
