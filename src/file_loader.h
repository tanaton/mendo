#pragma once
#include <string>
#include <functional>
#include <windows.h>

class FileLoader {
public:
    ~FileLoader();

    static std::string LoadFile(const std::wstring& path);
    static std::wstring OpenFileDialog(HWND owner);

    // File watching (timestamp polling)
    using ChangeCallback = std::function<void()>;
    void StartWatching(const std::wstring& file_path, ChangeCallback callback);
    void StopWatching() noexcept;
    void CheckForChanges();

private:
    std::wstring watch_path_;
    ChangeCallback on_change_;
    FILETIME last_write_time_{};
    bool watching_ = false;

    // Debounce: ignore changes within this window after a reload
    ULONGLONG last_reload_tick_ = 0;
    static constexpr DWORD DEBOUNCE_MS = 200;
};
