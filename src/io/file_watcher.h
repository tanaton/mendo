#pragma once
#include <string>
#include <functional>
#include <memory_resource>
#include <windows.h>
#include "win_handle.h"

// ReadDirectoryChangesW によるファイル変更監視
class FileWatcher {
public:
    ~FileWatcher();

    FileWatcher(const FileWatcher&) = delete;
    FileWatcher& operator=(const FileWatcher&) = delete;
    FileWatcher() = default;

    using ChangeCallback = std::move_only_function<void()>;
    void StartWatching(const std::pmr::wstring& file_path, ChangeCallback callback);
    void StopWatching() noexcept;
    void CheckForChanges();

    void ResumeWatching();
    HANDLE GetEventHandle() const noexcept;

private:
    void BeginRead();

    std::pmr::wstring watch_filename_;
    ChangeCallback on_change_;
    bool watching_ = false;

    UniqueHandle dir_handle_;
    UniqueEventHandle event_;
    OVERLAPPED overlapped_{};
    alignas(DWORD) char change_buf_[32768]{};
    bool read_pending_ = false;
    bool paused_ = false;
    bool pending_change_ = false;
};
