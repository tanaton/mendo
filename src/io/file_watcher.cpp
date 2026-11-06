#include "file_watcher.h"
#include "file_io.h"
#include "win32_util.h"
#include <cstring>

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

DirectoryChangeRead::~DirectoryChangeRead()
{
    if (pending) {
        // CancelIo はキャンセル要求のみで IO の終了を保証しない。CancelIoEx + GetOverlappedResult(..., TRUE)
        // でカーネルの completion routine が buf / overlapped への書き込みを終えるまで待つ。
        // 待たずに解放すると、解放した領域にカーネルが書き込みヒープ破壊を招く。
        CancelIoEx(dir.get(), &overlapped);
        DWORD bytes_returned = 0;
        GetOverlappedResult(dir.get(), &overlapped, &bytes_returned, TRUE);
    }
}

bool DirectoryChangeRead::Begin() noexcept
{
    ResetEvent(overlapped.hEvent);
    pending = ReadDirectoryChangesW(dir.get(), buf, sizeof(buf), FALSE, FILE_NOTIFY_CHANGE_LAST_WRITE | FILE_NOTIFY_CHANGE_FILE_NAME, nullptr,
                                    &overlapped, nullptr);
    return pending;
}

FileWatcher::~FileWatcher()
{
    StopWatching();
}

FileWatchTarget OpenWatchTarget(const std::wstring& file_path)
{
    FileWatchTarget target{ .filename = std::wstring(path_util::FilenameView(file_path)) };
    const auto short_path = QueryPath([&](wchar_t* b, UINT n) { return GetShortPathNameW(file_path.c_str(), b, n); });
    if (!short_path.empty()) {
        if (const auto short_name = path_util::FilenameView(short_path); !path_util::iequal(short_name, target.filename)) {
            target.short_filename = short_name;
        }
    }
    const auto dir = path_util::ParentDirectory(file_path);
    if (dir.empty()) {
        return target;
    }
    auto read = std::make_unique<DirectoryChangeRead>();
    read->dir.reset(CreateFileW(
        dir.c_str(),
        FILE_LIST_DIRECTORY,
        path_util::kFileShareRWDelete,
        nullptr,
        OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED,
        nullptr));
    read->event.reset(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    read->overlapped.hEvent = read->event.get();
    if (read->dir && read->event && read->Begin()) {
        target.read = std::move(read);
    }
    return target;
}

void FileWatcher::StartWatching(FileWatchTarget target, ChangeCallback callback)
{
    StopWatching();
    if (!target.read) {
        return;
    }
    on_change_ = std::move(callback);
    target_ = std::move(target);
}

void FileWatcher::StopWatching() noexcept
{
    target_.read.reset();
    paused_ = false;
    pending_change_ = false;
    on_change_ = nullptr;
}

void FileWatcher::CheckForChanges()
{
    auto* const read = target_.read.get();
    if (!read || !read->pending) {
        return;
    }

    DWORD bytes_returned = 0;
    if (!GetOverlappedResult(read->dir.get(), &read->overlapped, &bytes_returned, FALSE)) {
        if (GetLastError() != ERROR_IO_INCOMPLETE) {
            read->pending = false;
            StopWatching();
        }
        return;
    }

    read->pending = false;

    const bool target_changed = NotifyBufferHasTargetChange(
        std::as_bytes(std::span{ read->buf, bytes_returned }), target_.filename, target_.short_filename);
    if (target_changed) {
        if (paused_) {
            pending_change_ = true;
        }
        else {
            FireChange();
        }
    }
    // 通知を受けた側が監視をやめていることがあるので、引き直す。
    if (target_.read && !target_.read->Begin()) {
        StopWatching();
    }
}

void FileWatcher::FireChange()
{
    paused_ = true;
    on_change_();
}

void FileWatcher::ResumeWatching()
{
    paused_ = false;
    if (pending_change_) {
        pending_change_ = false;
        FireChange();
    }
}
