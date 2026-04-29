#include "file_watcher.h"
#include "path_util.h"
#include "file_io.h"
#include <filesystem>

FileWatcher::~FileWatcher()
{
    StopWatching();
}

void FileWatcher::StartWatching(const std::pmr::wstring& file_path, ChangeCallback callback)
{
    StopWatching();
    on_change_ = std::move(callback);

    const std::filesystem::path p(file_path);
    watch_filename_ = std::pmr::wstring{ p.filename().native() };

    const auto dir = p.parent_path();
    if (dir.empty()) {
        return;
    }

    dir_handle_.reset(CreateFileW(
        dir.c_str(),
        FILE_LIST_DIRECTORY,
        FILE_SHARE_RW_DELETE,
        nullptr,
        OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED,
        nullptr));

    if (!dir_handle_) {
        return;
    }

    event_.reset(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    if (!event_) {
        dir_handle_.reset();
        return;
    }

    overlapped_ = {};
    overlapped_.hEvent = event_.get();

    watching_ = true;
    BeginRead();
}

void FileWatcher::BeginRead()
{
    if (!dir_handle_) {
        return;
    }

    ResetEvent(overlapped_.hEvent);
    read_pending_ = ReadDirectoryChangesW(
        dir_handle_.get(),
        change_buf_,
        sizeof(change_buf_),
        FALSE,
        FILE_NOTIFY_CHANGE_LAST_WRITE | FILE_NOTIFY_CHANGE_FILE_NAME,
        nullptr,
        &overlapped_,
        nullptr);

    if (!read_pending_) {
        StopWatching();
    }
}

void FileWatcher::StopWatching() noexcept
{
    if (read_pending_ && dir_handle_) {
        CancelIo(dir_handle_.get());
        read_pending_ = false;
    }
    event_.reset();
    overlapped_ = {};
    dir_handle_.reset();
    watching_ = false;
    paused_ = false;
    pending_change_ = false;
    on_change_ = nullptr;
}

void FileWatcher::CheckForChanges()
{
    if (!watching_ || !read_pending_ || !dir_handle_) {
        return;
    }

    DWORD bytes_returned = 0;
    if (!GetOverlappedResult(dir_handle_.get(), &overlapped_, &bytes_returned, FALSE)) {
        if (GetLastError() != ERROR_IO_INCOMPLETE) {
            read_pending_ = false;
            StopWatching();
        }
        return;
    }

    read_pending_ = false;

    bool target_changed = false;
    if (bytes_returned > 0) {
        const auto* buf_end = reinterpret_cast<const char*>(change_buf_) + bytes_returned;
        auto* info = reinterpret_cast<FILE_NOTIFY_INFORMATION*>(change_buf_);
        for (;;) {
            const std::wstring_view changed_name{ info->FileName, info->FileNameLength / sizeof(wchar_t) };
            if (info->Action != FILE_ACTION_REMOVED &&
                info->Action != FILE_ACTION_RENAMED_OLD_NAME &&
                path_util::iequal(changed_name, watch_filename_)) {
                target_changed = true;
                break;
            }
            if (info->NextEntryOffset == 0) {
                break;
            }
            auto* next = reinterpret_cast<char*>(info) + info->NextEntryOffset;
            if (next + offsetof(FILE_NOTIFY_INFORMATION, FileName) > buf_end) {
                break;
            }
            info = reinterpret_cast<FILE_NOTIFY_INFORMATION*>(next);
        }
    }

    if (target_changed) {
        if (paused_) {
            pending_change_ = true;
        }
        else {
            paused_ = true;
            if (on_change_) {
                on_change_();
            }
        }
    }
    BeginRead();
}

HANDLE FileWatcher::GetEventHandle() const noexcept
{
    return (watching_ && read_pending_) ? overlapped_.hEvent : nullptr;
}

void FileWatcher::ResumeWatching()
{
    if (!watching_) {
        return;
    }
    paused_ = false;
    if (pending_change_) {
        pending_change_ = false;
        paused_ = true;
        if (on_change_) {
            on_change_();
        }
    }
}
