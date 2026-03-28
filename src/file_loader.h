#pragma once
#include <string>
#include <functional>
#include <windows.h>

class FileLoader {
public:
    ~FileLoader();

    static std::pmr::string LoadFile(const std::pmr::wstring& path);
    static std::pmr::wstring OpenFileDialog(HWND owner);

    // ファイル監視（ReadDirectoryChangesW による非同期監視）
    using ChangeCallback = std::function<void()>;
    void StartWatching(const std::pmr::wstring& file_path, ChangeCallback callback);
    void StopWatching() noexcept;
    void CheckForChanges();

private:
    void BeginRead();

    std::pmr::wstring watch_path_;
    std::pmr::wstring watch_filename_; // 監視対象のファイル名部分
    ChangeCallback on_change_;
    bool watching_ = false;

    // ReadDirectoryChangesW 用
    HANDLE dir_handle_ = INVALID_HANDLE_VALUE;
    OVERLAPPED overlapped_{};
    alignas(DWORD) char change_buf_[4096]{};
    bool read_pending_ = false;

    // デバウンス: リロード後の一定時間内の変更を無視
    ULONGLONG last_reload_tick_ = 0;
    static constexpr DWORD DEBOUNCE_MS = 200;
};
