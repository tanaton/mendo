#pragma once
#include <string>
#include <functional>
#include <windows.h>

class FileLoader {
public:
    ~FileLoader();

    static std::pmr::string LoadFile(const std::pmr::wstring& path);
    static std::pmr::wstring OpenFileDialog(HWND owner);

    // ファイル監視（タイムスタンプポーリング）
    using ChangeCallback = std::function<void()>;
    void StartWatching(const std::pmr::wstring& file_path, ChangeCallback callback);
    void StopWatching() noexcept;
    void CheckForChanges();

private:
    std::pmr::wstring watch_path_;
    ChangeCallback on_change_;
    FILETIME last_write_time_{};
    bool watching_ = false;

    // デバウンス: リロード後の一定時間内の変更を無視
    ULONGLONG last_reload_tick_ = 0;
    static constexpr DWORD DEBOUNCE_MS = 200;
};
