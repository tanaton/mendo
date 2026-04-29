#pragma once
#include "document.h"
#include "file_watcher.h"
#include "file_loader.h"
#include <expected>

class DocumentService {
public:
    explicit DocumentService(FileWatcher& watcher) noexcept : watcher_(watcher) {}

    std::expected<Document, FileLoadError> LoadFile(const std::pmr::wstring& path);
    void StartWatching(const std::pmr::wstring& path, FileWatcher::ChangeCallback cb);
    void StopWatching() noexcept;
    void CheckForChanges();
    void ResumeWatching();
    constexpr HANDLE GetFileWatchEvent() const noexcept { return watcher_.GetEventHandle(); }

    static bool NeedsAsyncLoad(const std::pmr::wstring& path) noexcept;
    static bool NeedsLoadingAnimation(const std::pmr::wstring& path) noexcept;

private:
    FileWatcher& watcher_;
};
