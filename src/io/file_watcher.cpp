#include "file_watcher.h"
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

    // bytes_returned == 0 はバッファ溢れでカーネルが変更内容を破棄した状態。取りこぼし回避のため変更ありとして扱う。
    const bool target_changed = bytes_returned == 0 || IsTargetChanged(bytes_returned);
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

bool FileWatcher::IsTargetChanged(DWORD bytes_returned) const noexcept
{
    const char* const buf_end = change_buf_ + bytes_returned;
    const char* cur = change_buf_;
    for (;;) {
        // カーネルが切り詰めた通知に備え、現在エントリの範囲を buf_end で検証してから
        // 参照する (NextEntryOffset の検証は次エントリ用で先頭エントリを守らない)。
        const char* name_begin = cur + offsetof(FILE_NOTIFY_INFORMATION, FileName);
        if (name_begin > buf_end) {
            return false;
        }
        const auto* info = reinterpret_cast<const FILE_NOTIFY_INFORMATION*>(cur);
        const size_t name_bytes = info->FileNameLength;
        // 巨大/破損した FileNameLength で OOB ポインタ (name_begin + name_bytes) を
        // 形成する前に、残りバッファ長と byte 数を比較する。
        if (name_bytes > static_cast<size_t>(buf_end - name_begin)) {
            return false;
        }
        if (info->Action != FILE_ACTION_REMOVED && info->Action != FILE_ACTION_RENAMED_OLD_NAME &&
            MatchesWatchedName({ info->FileName, name_bytes / sizeof(wchar_t) })) {
            return true;
        }
        const size_t next_off = info->NextEntryOffset;
        if (next_off == 0) {
            return false;
        }
        // 次エントリも OOB ポインタを作る前に、残りバッファ長で NextEntryOffset を
        // 検証する (固定部 offsetof(FileName) が収まることも要求する)。
        const size_t remaining = static_cast<size_t>(buf_end - cur);
        if (next_off > remaining || remaining - next_off < offsetof(FILE_NOTIFY_INFORMATION, FileName)) {
            return false;
        }
        cur += next_off;
    }
}

bool FileWatcher::MatchesWatchedName(std::wstring_view changed_name) const noexcept
{
    return path_util::iequal(changed_name, watch_filename_) ||
        (!watch_filename_short_.empty() && path_util::iequal(changed_name, watch_filename_short_));
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
