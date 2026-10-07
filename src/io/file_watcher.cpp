#include "file_watcher.h"
#include "file_io.h"
#include <cstring>
#include <filesystem>

bool NotifyBufferHasTargetChange(std::span<const std::byte> buf, std::wstring_view name, std::wstring_view short_name) noexcept
{
    if (buf.empty()) {
        return true;
    }
    constexpr size_t kHeaderBytes = offsetof(FILE_NOTIFY_INFORMATION, FileName);
    size_t offset = 0;
    for (;;) {
        // カーネルが切り詰めた通知に備え、現在エントリの固定部と名前が buf に収まることを
        // 参照前に検証する (NextEntryOffset の検証だけでは先頭エントリを守れない)。
        const size_t remaining = buf.size() - offset;
        if (remaining < kHeaderBytes) {
            return false;
        }
        FILE_NOTIFY_INFORMATION header;
        std::memcpy(&header, buf.data() + offset, kHeaderBytes);
        const size_t name_bytes = header.FileNameLength;
        if (name_bytes > remaining - kHeaderBytes) {
            return false;
        }
        if (header.Action != FILE_ACTION_REMOVED && header.Action != FILE_ACTION_RENAMED_OLD_NAME) {
            const std::wstring_view changed{ reinterpret_cast<const wchar_t*>(buf.data() + offset + kHeaderBytes), name_bytes / sizeof(wchar_t) };
            if (path_util::iequal(changed, name) || (!short_name.empty() && path_util::iequal(changed, short_name))) {
                return true;
            }
        }
        const size_t next = header.NextEntryOffset;
        if (next == 0 || next > remaining) {
            return false;
        }
        offset += next;
    }
}

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

    watch_filename_short_.clear();
    wchar_t short_buf[MAX_PATH];
    const DWORD short_len = GetShortPathNameW(file_path.c_str(), short_buf, MAX_PATH);
    if (short_len > 0 && short_len < MAX_PATH) {
        std::pmr::wstring short_name{ std::filesystem::path(short_buf).filename().native() };
        if (!path_util::iequal(short_name, watch_filename_)) {
            watch_filename_short_ = std::move(short_name);
        }
    }

    const auto dir = p.parent_path();
    if (dir.empty()) {
        return;
    }

    dir_handle_.reset(CreateFileW(
        dir.c_str(),
        FILE_LIST_DIRECTORY,
        path_util::kFileShareRWDelete,
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
        // CancelIo はキャンセル要求のみで IO の終了を保証しない。CancelIoEx + GetOverlappedResult(..., TRUE)
        // でカーネルの completion routine が change_buf_ / overlapped_ への書き込みを終えるまで待つ。
        // 待たずに event_.reset() / overlapped_={} すると次回 StartWatching の change_buf_ 再利用と race し
        // ヒープ破壊や謎のシグナルを招く。
        CancelIoEx(dir_handle_.get(), &overlapped_);
        DWORD bytes_returned = 0;
        GetOverlappedResult(dir_handle_.get(), &overlapped_, &bytes_returned, TRUE);
        read_pending_ = false;
    }
    event_.reset();
    overlapped_ = {};
    dir_handle_.reset();
    paused_ = false;
    pending_change_ = false;
    on_change_ = nullptr;
}

void FileWatcher::CheckForChanges()
{
    if (!read_pending_) {
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

    const bool target_changed = NotifyBufferHasTargetChange(
        std::as_bytes(std::span{ change_buf_, bytes_returned }), watch_filename_, watch_filename_short_);
    if (target_changed) {
        if (paused_) {
            pending_change_ = true;
        }
        else {
            FireChange();
        }
    }
    BeginRead();
}

void FileWatcher::FireChange()
{
    paused_ = true;
    if (on_change_) {
        on_change_();
    }
}

void FileWatcher::ResumeWatching()
{
    if (!dir_handle_) {
        return;
    }
    paused_ = false;
    if (pending_change_) {
        pending_change_ = false;
        FireChange();
    }
}
