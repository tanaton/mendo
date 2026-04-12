#pragma once
#include "document.h"
#include "file_watcher.h"
#include "file_loader.h"
#include <expected>

class DocumentService {
public:
    explicit DocumentService(FileWatcher& watcher) noexcept : watcher_(watcher) {}

    // ファイルを読み込み、Document を構築。
    std::expected<Document, FileLoadError> LoadFile(const std::pmr::wstring& path);

    // ファイル監視
    void StartWatching(const std::pmr::wstring& path, FileWatcher::ChangeCallback cb);
    void StopWatching() noexcept;
    void CheckForChanges();
    void ResumeWatching();
    HANDLE GetFileWatchEvent() const noexcept { return watcher_.GetEventHandle(); }

    // パース時間がUIブロックとして体感されるサイズか（非同期ロード判定用）
    static bool NeedsAsyncLoad(const std::pmr::wstring& path) noexcept;

    // 非常に大きいファイルか（ローディングアニメーション表示判定用）
    static bool NeedsLoadingAnimation(const std::pmr::wstring& path) noexcept;

private:
    FileWatcher& watcher_;
};
