#pragma once
#include "win_handle.h"
#include <functional>
#include <memory_resource>
#include <string>
#include <string_view>
#include <windows.h>

class FileWatcher {
public:
    FileWatcher() = default;
    ~FileWatcher();

    FileWatcher(const FileWatcher&) = delete;
    FileWatcher& operator=(const FileWatcher&) = delete;

    using ChangeCallback = std::move_only_function<void()>;
    void StartWatching(const std::pmr::wstring& file_path, ChangeCallback callback);
    void StopWatching() noexcept;
    void CheckForChanges();

    void ResumeWatching();
    constexpr HANDLE GetEventHandle() const noexcept
    {
        return read_pending_ ? overlapped_.hEvent : nullptr;
    }

private:
    void BeginRead();
    // change_buf_ の通知列に監視対象の更新 (削除・改名元以外) が含まれるか。
    bool IsTargetChanged(DWORD bytes_returned) const noexcept;
    bool MatchesWatchedName(std::wstring_view changed_name) const noexcept;
    // 通知後は ResumeWatching まで paused_ で後続通知を pending_change_ に溜める。
    void FireChange();

    std::pmr::wstring watch_filename_;
    // 通知は 8.3 短縮名で来ることがある (未規定)。長い名前と一致しない場合のみ保持。
    std::pmr::wstring watch_filename_short_;
    ChangeCallback on_change_;

    UniqueHandle dir_handle_;
    UniqueEventHandle event_;
    OVERLAPPED overlapped_{};
    // ReadDirectoryChangesW はバッファ溢れで以降の通知を失うため余裕を持って 64KB 確保。
    static constexpr size_t CHANGE_BUF_SIZE = 64 * 1024;
    alignas(DWORD) char change_buf_[CHANGE_BUF_SIZE]{};
    // true なら dir_handle_ も有効。
    bool read_pending_ = false;
    bool paused_ = false;
    bool pending_change_ = false;
};
