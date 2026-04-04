#include "file_watcher.h"
#include <filesystem>

FileWatcher::~FileWatcher()
{
    StopWatching();
}

void FileWatcher::StartWatching(const std::pmr::wstring& file_path, ChangeCallback callback)
{
    StopWatching();
    on_change_ = std::move(callback);
    last_reload_tick_ = GetTickCount64();

    const std::filesystem::path p(file_path);
    watch_filename_ = std::pmr::wstring{ p.filename().native() };

    const auto dir = p.parent_path();
    if (dir.empty()) {
        return;
    }

    dir_handle_ = CreateFileW(
        dir.c_str(),
        FILE_LIST_DIRECTORY,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr,
        OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED,
        nullptr);

    if (dir_handle_ == INVALID_HANDLE_VALUE) {
        return;
    }

    overlapped_ = {};
    overlapped_.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!overlapped_.hEvent) {
        CloseHandle(dir_handle_);
        dir_handle_ = INVALID_HANDLE_VALUE;
        return;
    }

    watching_ = true;
    BeginRead();
}

void FileWatcher::BeginRead()
{
    if (dir_handle_ == INVALID_HANDLE_VALUE) {
        return;
    }

    ResetEvent(overlapped_.hEvent);
    read_pending_ = ReadDirectoryChangesW(
        dir_handle_,
        change_buf_,
        sizeof(change_buf_),
        FALSE,
        FILE_NOTIFY_CHANGE_LAST_WRITE | FILE_NOTIFY_CHANGE_SIZE | FILE_NOTIFY_CHANGE_FILE_NAME,
        nullptr,
        &overlapped_,
        nullptr);

    if (!read_pending_) {
        StopWatching();
    }
}

void FileWatcher::StopWatching() noexcept
{
    if (read_pending_ && dir_handle_ != INVALID_HANDLE_VALUE) {
        CancelIo(dir_handle_);
        read_pending_ = false;
    }
    if (overlapped_.hEvent) {
        CloseHandle(overlapped_.hEvent);
        overlapped_ = {};
    }
    if (dir_handle_ != INVALID_HANDLE_VALUE) {
        CloseHandle(dir_handle_);
        dir_handle_ = INVALID_HANDLE_VALUE;
    }
    watching_ = false;
    on_change_ = nullptr;
}

void FileWatcher::CheckForChanges()
{
    if (!watching_ || !read_pending_ || dir_handle_ == INVALID_HANDLE_VALUE) {
        return;
    }

    DWORD bytes_returned = 0;
    if (!GetOverlappedResult(dir_handle_, &overlapped_, &bytes_returned, FALSE)) {
        return;
    }

    read_pending_ = false;

    const ULONGLONG now = GetTickCount64();
    const bool debounced = (now - last_reload_tick_ < DEBOUNCE_MS);

    bool target_changed = false;
    if (bytes_returned > 0 && !debounced) {
        auto* info = reinterpret_cast<FILE_NOTIFY_INFORMATION*>(change_buf_);
        for (;;) {
            const std::wstring_view changed_name(info->FileName, info->FileNameLength / sizeof(wchar_t));
            if (changed_name.size() == watch_filename_.size() &&
                _wcsnicmp(changed_name.data(), watch_filename_.c_str(), changed_name.size()) == 0) {
                target_changed = true;
                break;
            }
            if (info->NextEntryOffset == 0) {
                break;
            }
            info = reinterpret_cast<FILE_NOTIFY_INFORMATION*>(
                reinterpret_cast<char*>(info) + info->NextEntryOffset);
        }
    }

    BeginRead();

    if (target_changed) {
        last_reload_tick_ = now;
        if (on_change_) {
            on_change_();
        }
    }
}

void FileWatcher::ResetDebounceTick() noexcept
{
    last_reload_tick_ = GetTickCount64();
}
