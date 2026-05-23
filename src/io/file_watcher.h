#pragma once
#include <string>
#include <functional>
#include <memory_resource>
#include <windows.h>
#include "win_handle.h"

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
    constexpr HANDLE GetEventHandle() const noexcept
    {
        return (watching_ && read_pending_) ? overlapped_.hEvent : nullptr;
    }

private:
    void BeginRead();

    std::pmr::wstring watch_filename_;
    ChangeCallback on_change_;
    bool watching_ = false;

    UniqueHandle dir_handle_;
    UniqueEventHandle event_;
    OVERLAPPED overlapped_{};
    // ReadDirectoryChangesW はバッファ溢れで以降の通知を失うため余裕を持って 64KB 確保。
    static constexpr size_t CHANGE_BUF_SIZE = 64 * 1024;
    alignas(DWORD) char change_buf_[CHANGE_BUF_SIZE]{};
    bool read_pending_ = false;
    bool paused_ = false;
    bool pending_change_ = false;
};
