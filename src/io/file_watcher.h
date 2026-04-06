#pragma once
#include <string>
#include <functional>
#include <memory_resource>
#include <windows.h>

// ReadDirectoryChangesW によるファイル変更監視。
// 指定ファイルの親ディレクトリを監視し、対象ファイルの変更を検出してコールバックを呼ぶ。
// デバウンス機能付き。
class FileWatcher {
public:
    ~FileWatcher();

    FileWatcher(const FileWatcher&) = delete;
    FileWatcher& operator=(const FileWatcher&) = delete;
    FileWatcher() = default;

    using ChangeCallback = std::function<void()>;
    void StartWatching(const std::pmr::wstring& file_path, ChangeCallback callback);
    void StopWatching() noexcept;
    void CheckForChanges();
    void ResetDebounceTick() noexcept;

private:
    void BeginRead();

    std::pmr::wstring watch_filename_;
    ChangeCallback on_change_;
    bool watching_ = false;

    HANDLE dir_handle_ = INVALID_HANDLE_VALUE;
    OVERLAPPED overlapped_{};
    alignas(DWORD) char change_buf_[4096]{};
    bool read_pending_ = false;

    ULONGLONG last_reload_tick_ = 0;
    static constexpr DWORD DEBOUNCE_MS = 200;
};
