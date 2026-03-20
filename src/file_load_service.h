#pragma once
#include "document_service.h"
#include "document.h"
#include "layout_cache.h"
#include <string>

// Manages file loading orchestration and loading animation state.
// No Win32 dependency — fully testable.
class FileLoadService {
public:
    explicit FileLoadService(DocumentService& doc_service) noexcept;

    // ---- Loading animation state ----

    bool IsLoading() const noexcept { return loading_; }
    float GetLoadingAngle() const noexcept { return loading_angle_; }

    // Start loading animation for a file.
    void StartLoading(const std::wstring& path);

    // Stop loading animation.
    void StopLoading() noexcept;

    // Advance loading animation by one frame.
    void TickLoadingAnimation() noexcept;

    // ---- File loading ----

    // Execute file load using the stored loading path.
    // Returns true on success. Caller should read doc for directory/file_path.
    bool ExecuteLoad(Document& doc, LayoutCache& cache);

    // Reload current file.
    bool ExecuteReload(Document& doc, LayoutCache& cache);

    // ---- Path access ----

    const std::wstring& GetLoadingPath() const noexcept { return loading_path_; }
    void SetLoadingPath(const std::wstring& path) { loading_path_ = path; }

private:
    DocumentService& doc_service_;
    bool loading_ = false;
    float loading_angle_ = 0.0f;
    std::wstring loading_path_;
};
