#pragma once
#include <string>
#include <functional>
#include <memory_resource>
#include <windows.h>
#include "win_handle.h"

// ReadDirectoryChangesW によるファイル変更監視。
// 指定ファイルの親ディレクトリを監視し、対象ファイルの変更を検出してコールバックを呼ぶ。
// 変更検出後はコールバック呼び出しを一時停止し、ResumeWatching() で再開する。
// 一時停止中も I/O は継続し、追加の変更は蓄積される。
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

    // 変更検出後に一時停止した監視を再開する。
    // リロード完了後に呼び出すこと。
    void ResumeWatching();

    // MsgWaitForMultipleObjects に渡す待機ハンドルを返す。
    // 監視中でなければ nullptr を返す。
    HANDLE GetEventHandle() const noexcept;

private:
    void BeginRead();

    std::pmr::wstring watch_filename_;
    ChangeCallback on_change_;
    bool watching_ = false;

    UniqueHandle dir_handle_;
    UniqueEventHandle event_;          // overlapped_.hEvent のオーナー（StartWatching で同期）
    OVERLAPPED overlapped_{};
    alignas(DWORD) char change_buf_[4096]{};
    bool read_pending_ = false;
    bool paused_ = false;
    bool pending_change_ = false;
};
