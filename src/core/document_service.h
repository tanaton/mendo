#pragma once
#include "document.h"
#include "file_watcher.h"

class DocumentService {
public:
    explicit DocumentService(FileWatcher& watcher) noexcept : watcher_(watcher) {}

    // ファイルを読み込み、Document を構築。成功時 true。
    bool LoadFile(const std::pmr::wstring& path, Document& doc);

    // ファイル監視
    void StartWatching(const std::pmr::wstring& path, FileWatcher::ChangeCallback cb);
    void StopWatching() noexcept;
    void CheckForChanges();
    void ResumeWatching();
    HANDLE GetFileWatchEvent() const noexcept { return watcher_.GetEventHandle(); }

    // 大きいファイルかどうか（ローディングアニメ判定用）
    static bool NeedsLoadingAnimation(const std::pmr::wstring& path) noexcept;

private:
    FileWatcher& watcher_;
};
