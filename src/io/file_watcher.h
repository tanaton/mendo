#pragma once
#include "win_handle.h"
#include <functional>
#include <cstddef>
#include <memory_resource>
#include <span>
#include <string>
#include <string_view>
#include <windows.h>

// ReadDirectoryChangesW の通知列に監視対象 (name、または空でなければ 8.3 短縮名 short_name。大小無視) の
// 更新 (削除・改名元以外) が含まれるか。空の buf はカーネルが内容を捨てたバッファ溢れなので取りこぼし回避で true。
// 切り詰め・破損したエントリに当たったらそこで打ち切り、buf の外は読まない。
[[nodiscard]] bool NotifyBufferHasTargetChange(std::span<const std::byte> buf, std::wstring_view name, std::wstring_view short_name) noexcept;

// 監視を始めるのに要るファイルシステムへの問い合わせ (短縮名と親フォルダを開くこと) の結果。
// ネットワーク上のファイルではパスの要素ごとに往復が入る (ループバックの SMB でもローカルの約 5 倍、約 1ms) ので、
// 読み込みの worker で作り (OpenWatchTarget)、UI スレッドの StartWatching に渡す。
struct FileWatchTarget {
    std::pmr::wstring filename;
    // 通知は 8.3 短縮名で来ることがある (未規定)。長い名前と一致しない場合のみ持つ。
    std::pmr::wstring short_filename;
    // 親フォルダ。開けなければ無効で、監視しない。
    UniqueHandle dir;
};
[[nodiscard]] FileWatchTarget OpenWatchTarget(const std::pmr::wstring& file_path);

class FileWatcher {
public:
    FileWatcher() = default;
    ~FileWatcher();

    FileWatcher(const FileWatcher&) = delete;
    FileWatcher& operator=(const FileWatcher&) = delete;

    using ChangeCallback = std::move_only_function<void()>;
    // 今の監視をやめ、target を監視する。target.dir が無効なら監視しない。
    void StartWatching(FileWatchTarget target, ChangeCallback callback);
    void StopWatching() noexcept;
    void CheckForChanges();

    void ResumeWatching();
    // paused 中に溜めた通知を捨てる。読み直しを始める直前に呼び、その読み込みに含まれる変更で再発火させない。
    constexpr void DiscardPendingChange() noexcept
    {
        pending_change_ = false;
    }
    // 通知済みの変更を読まずに取りやめたときに呼び、次の ResumeWatching で通知し直させる。
    constexpr void RequeueIfPaused() noexcept
    {
        if (paused_) {
            pending_change_ = true;
        }
    }
    constexpr HANDLE GetEventHandle() const noexcept
    {
        return read_pending_ ? overlapped_.hEvent : nullptr;
    }

private:
    void BeginRead();
    // 通知後は ResumeWatching まで paused_ で後続通知を pending_change_ に溜める。
    void FireChange();

    std::pmr::wstring watch_filename_;
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
