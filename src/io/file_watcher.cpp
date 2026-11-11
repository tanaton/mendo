#include "file_watcher.h"
#include "file_io.h"
#include "task_scheduler.h"
#include "win32_util.h"
#include <algorithm>
#include <cstring>
#include <utility>

namespace {

// 通知列のうち更新 (削除・改名元以外) の名前を順に on_name へ渡し、true が返ったら打ち切って true を返す。
// 切り詰め・破損したエントリに当たったらそこで打ち切り、buf の外は読まない。
template <class F>
bool AnyUpdatedName(std::span<const std::byte> buf, F&& on_name)
{
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
            if (on_name(changed)) {
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

std::unique_ptr<DirectoryChangeRead> OpenDirectoryRead(const std::wstring& dir)
{
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
        return read;
    }
    return nullptr;
}

} // namespace

bool NotifyBufferHasTargetChange(std::span<const std::byte> buf, std::wstring_view name, std::wstring_view short_name) noexcept
{
    return buf.empty() || AnyUpdatedName(buf, [&](std::wstring_view changed) noexcept {
        return path_util::iequal(changed, name) || (!short_name.empty() && path_util::iequal(changed, short_name));
    });
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

DirectoryChangeRead::Poll DirectoryChangeRead::Take(std::span<const std::byte>& notified) noexcept
{
    if (!pending) {
        return Poll::Waiting;
    }
    DWORD bytes_returned = 0;
    if (!GetOverlappedResult(dir.get(), &overlapped, &bytes_returned, FALSE)) {
        if (GetLastError() == ERROR_IO_INCOMPLETE) {
            return Poll::Waiting;
        }
        pending = false;
        return Poll::Failed;
    }
    pending = false;
    notified = std::as_bytes(std::span{ buf, bytes_returned });
    return Poll::Ready;
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
    target.read = OpenDirectoryRead(dir);
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
    if (target_.read && disposer_) {
        DisposeOnWorkerIfLarge(*disposer_, 1, 1, std::move(target_.read));
    }
    target_.read.reset();
    on_change_ = nullptr;
}

void FileWatcher::CheckForChanges()
{
    auto* const read = target_.read.get();
    if (!read) {
        return;
    }
    using Poll = DirectoryChangeRead::Poll;
    std::span<const std::byte> notified;
    const Poll poll = read->Take(notified);
    if (poll != Poll::Ready) {
        if (poll == Poll::Failed) {
            StopWatching();
        }
        return;
    }

    if (NotifyBufferHasTargetChange(notified, target_.filename, target_.short_filename)) {
        on_change_();
    }
    // 通知を受けた側が監視をやめたか替えたなら引き直さない。替えた対象は読み取りを発行済みで、同じ overlapped に重ねて発行できない
    // (新しい対象は前の対象が生きている間に作るので、アドレスは重ならない)。
    if (target_.read.get() == read && !read->Begin()) {
        StopWatching();
    }
}

ImageFolderWatcher::Inbox::Inbox() : event(CreateEventW(nullptr, TRUE, FALSE, nullptr))
{}

void ImageFolderWatcher::Inbox::Put(WorkerResult result)
{
    if (results.Push(std::move(result))) {
        SetEvent(event.get());
    }
}

std::vector<ImageFolderWatcher::WorkerResult> ImageFolderWatcher::Inbox::Take()
{
    // 倒してから取り出す。逆だと、その間に空の受け箱へ入った結果のシグナルを消してしまう。
    // この順では、取り出した結果のシグナルが残って空の取り出しが 1 回余分に起きることがあるだけ。
    ResetEvent(event.get());
    return results.TakeAll();
}

bool ImageFolderWatcher::PostWork(std::move_only_function<void(Inbox&)> task)
{
    if (!inbox_) {
        inbox_ = std::make_shared<Inbox>();
    }
    if (!scheduler_->Post([inbox = inbox_, task = std::move(task)]() mutable { task(*inbox); })) {
        return false;
    }
    ++awaited_results_;
    return true;
}

template <class T>
void ImageFolderWatcher::DisposeOnWorker(T obj) noexcept
{
    DisposeOnWorkerIfLarge(*scheduler_, 1, 1, std::move(obj));
}

void ImageFolderWatcher::Watch(const std::wstring& abs_path, TaskScheduler* scheduler)
{
    auto dir = path_util::ParentDirectory(abs_path);
    if (dir.empty() || !scheduler) {
        return;
    }
    scheduler_ = scheduler;
    auto it = std::ranges::find_if(folders_, [&](const Folder& f) { return path_util::iequal(f.dir, dir); });
    if (it == folders_.end()) {
        if (folders_.size() >= kMaxFolders) {
            return;
        }
        const uint64_t id = next_folder_id_++;
        const bool posted = PostWork([id, dir](Inbox& inbox) { inbox.Put({ .folder_id = id, .opened = true, .read = OpenDirectoryRead(dir) }); });
        it = folders_.insert(folders_.end(), Folder{ .id = id, .dir = std::move(dir), .opening = posted });
    }
    if (it->opening || it->read) {
        it->paths.push_back(abs_path);
    }
}

bool ImageFolderWatcher::IsOpening(std::wstring_view abs_path) const noexcept
{
    // 画像の走査のたびに画像ごとに呼ばれる。開いている最中のフォルダは結果を待っているので、待ちが無ければパスを見ずに返す。
    if (awaited_results_ == 0) {
        return false;
    }
    // パスをフォルダに分けず (確保が要る)、フォルダの後ろがファイル名だけかを見る。
    const size_t name_size = path_util::FilenameView(abs_path).size();
    return std::ranges::any_of(folders_, [&](const Folder& f) {
        if (!f.opening || abs_path.size() <= f.dir.size() || !path_util::iequal(abs_path.substr(0, f.dir.size()), f.dir)) {
            return false;
        }
        // フォルダが区切りで終わる (ドライブの直下) なら、その後ろがすぐファイル名。
        const size_t rest = abs_path.size() - f.dir.size();
        return rest == name_size || (rest == name_size + 1 && path_util::IsPathSlash(abs_path[f.dir.size()]));
    });
}

void ImageFolderWatcher::Clear() noexcept
{
    if (!folders_.empty()) {
        DisposeOnWorker(std::exchange(folders_, {}));
    }
}

size_t ImageFolderWatcher::EventHandles(std::span<HANDLE> out) const noexcept
{
    size_t count = 0;
    if (awaited_results_ > 0) {
        out[count++] = inbox_->event.get();
    }
    for (const Folder& f : folders_) {
        if (f.read && f.read->pending) {
            out[count++] = f.read->event.get();
        }
    }
    return count;
}

bool ImageFolderWatcher::AddNamed(const Folder& folder, std::wstring_view name, std::vector<std::wstring>& changed)
{
    // フォルダのほかのファイルの通知でも登録した画像を全部見るので、大小無視の比較の前に長さで弾く。
    bool found = false;
    for (const std::wstring& path : folder.paths) {
        if (const auto watched = path_util::FilenameView(path); watched.size() == name.size() && path_util::iequal(watched, name)) {
            changed.push_back(path);
            found = true;
        }
    }
    return found;
}

bool ImageFolderWatcher::CheckForChanges(std::vector<std::wstring>& changed)
{
    bool any_opened = false;
    if (awaited_results_ > 0) {
        for (WorkerResult& result : inbox_->Take()) {
            --awaited_results_;
            const auto it = std::ranges::find(folders_, result.folder_id, &Folder::id);
            if (it == folders_.end()) {
                // Clear でやめたフォルダの結果。
                if (result.read) {
                    DisposeOnWorker(std::move(result.read));
                }
                continue;
            }
            if (result.opened) {
                it->opening = false;
                it->read = std::move(result.read);
                if (!it->read) {
                    it->paths.clear();
                }
                any_opened = true;
            }
            else {
                AddNamed(*it, result.long_name, changed);
            }
        }
    }

    using Poll = DirectoryChangeRead::Poll;
    for (Folder& f : folders_) {
        std::span<const std::byte> notified;
        const Poll poll = f.read ? f.read->Take(notified) : Poll::Waiting;
        if (poll == Poll::Waiting) {
            continue;
        }
        if (poll == Poll::Failed) {
            // フォルダが消えたなど。このフォルダの監視をやめる。
            DisposeOnWorker(std::move(f.read));
            continue;
        }
        if (notified.empty()) {
            // カーネルが内容を捨てたバッファ溢れ。どの画像が変わったか分からない。
            changed.append_range(f.paths);
        }
        else {
            AnyUpdatedName(notified, [&](std::wstring_view name) {
                // 通知は 8.3 短縮名で来ることがある (未規定)。短縮名を画像ごとに前もって問い合わせると枚数ぶんの往復に
                // なるので、当たらなかった短縮名の形 (12 文字以内で ~ と数字を含む) の名前だけ長い名前に直して照らす。
                // ~ だけで問い合わせると、エディタの一時ファイル (~$x.docx・x.md~) の通知のたびに走る。
                // 問い合わせはファイルに触れるので worker でし、結果は次の CheckForChanges で照らす。
                const size_t tilde = name.find(L'~');
                const bool short_form = name.size() <= 12 && tilde != name.npos && tilde + 1 < name.size() && ascii_util::IsAsciiDigit(name[tilde + 1]);
                if (!AddNamed(f, name, changed) && short_form) {
                    PostWork([id = f.id, short_path = f.dir + L'\\' + std::wstring{ name }](Inbox& inbox) {
                        const auto long_path = QueryPath([&](wchar_t* b, UINT n) { return GetLongPathNameW(short_path.c_str(), b, n); });
                        inbox.Put({ .folder_id = id, .long_name = std::wstring{ path_util::FilenameView(long_path) } });
                    });
                }
                return false;
            });
        }
        if (!f.read->Begin()) {
            DisposeOnWorker(std::move(f.read));
        }
    }
    return any_opened;
}
