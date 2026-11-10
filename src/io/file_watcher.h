#pragma once
#include "win_handle.h"
#include <functional>
#include <cstddef>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <windows.h>

// ReadDirectoryChangesW の通知列に監視対象 (name、または空でなければ 8.3 短縮名 short_name。大小無視) の
// 更新 (削除・改名元以外) が含まれるか。空の buf はカーネルが内容を捨てたバッファ溢れなので取りこぼし回避で true。
// 切り詰め・破損したエントリに当たったらそこで打ち切り、buf の外は読まない。
[[nodiscard]] bool NotifyBufferHasTargetChange(std::span<const std::byte> buf, std::wstring_view name, std::wstring_view short_name) noexcept;

// 親フォルダと、その上で発行した ReadDirectoryChangesW の読み取り。カーネルが overlapped と buf に書くので動かさず、
// ヒープに置いて持ち回る。読み取りを発行したまま壊すと、取り消して書き終わるのを待つ。
struct DirectoryChangeRead {
    DirectoryChangeRead() = default;
    ~DirectoryChangeRead();
    DirectoryChangeRead(const DirectoryChangeRead&) = delete;
    DirectoryChangeRead& operator=(const DirectoryChangeRead&) = delete;

    // 次の読み取りを発行する。発行できなければ false。
    bool Begin() noexcept;

    UniqueHandle dir;
    UniqueNullHandle event;
    OVERLAPPED overlapped{};
    // 溢れると貯まった通知が捨てられ (0 バイトで返り、変更として扱う)、無関係な変更の多いフォルダで読み直しが増える。
    // ネットワーク越しで許される上限の 64KB にする。
    static constexpr size_t BUF_SIZE = 64 * 1024;
    alignas(DWORD) char buf[BUF_SIZE]{};
    bool pending = false;
};

// 監視の対象。ReadDirectoryChangesW は最初に呼んだ後の変更を貯めるので、読み込みの worker がファイルを読む前に作り
// (OpenWatchTarget)、UI スレッドの StartWatching に渡す。読んでから監視を始めるまでの保存も、ほかの変更と同じく通知で届く。
// 短縮名の問い合わせと親フォルダを開くことは、ネットワーク上のファイルではパスの要素ごとに往復が入る
// (ループバックの SMB でもローカルの約 5 倍、約 1ms) ので、これも worker で済ませる。
struct FileWatchTarget {
    std::wstring filename;
    // 通知は 8.3 短縮名で来ることがある (未規定)。長い名前と一致しない場合のみ持つ。
    std::wstring short_filename;
    // 親フォルダを開けないか読み取りを発行できなければ null で、監視しない。
    std::unique_ptr<DirectoryChangeRead> read;
};
[[nodiscard]] FileWatchTarget OpenWatchTarget(const std::wstring& file_path);

class FileWatcher {
public:
    FileWatcher() = default;
    ~FileWatcher();

    FileWatcher(const FileWatcher&) = delete;
    FileWatcher& operator=(const FileWatcher&) = delete;

    // 変更のたびに呼ぶ。読み直しの最中かどうかは呼ばれた側 (ReloadScheduler) が見る。
    using ChangeCallback = std::move_only_function<void()>;
    // 今の監視をやめ、target を監視する。target.read が無ければ監視しない。
    void StartWatching(FileWatchTarget target, ChangeCallback callback);
    void StopWatching() noexcept;
    void CheckForChanges();

    HANDLE GetEventHandle() const noexcept
    {
        return target_.read && target_.read->pending ? target_.read->event.get() : nullptr;
    }

private:
    FileWatchTarget target_;
    ChangeCallback on_change_;
};
