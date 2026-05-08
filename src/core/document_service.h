#pragma once
#include "document.h"
#include "file_watcher.h"
#include "file_loader.h"
#include <expected>

class DocumentService {
public:
    explicit DocumentService(FileWatcher& watcher) noexcept : watcher_(watcher)
    {}

    static std::expected<Document, FileLoadError> LoadFile(const std::pmr::wstring& path);
    void StartWatching(const std::pmr::wstring& path, FileWatcher::ChangeCallback cb);
    void StopWatching() noexcept;
    void CheckForChanges();
    void ResumeWatching();
    constexpr HANDLE GetFileWatchEvent() const noexcept
    {
        return watcher_.GetEventHandle();
    }

    // path のファイルサイズが threshold を超える場合 true。同期/非同期ロード判定および
    // ローディングアニメーション表示判定に共通で使う。しきい値定数は app_constants.h::app_threshold。
    static bool IsLargerThan(const std::pmr::wstring& path, DWORD threshold) noexcept;

private:
    FileWatcher& watcher_;
};
