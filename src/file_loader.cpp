#include "file_loader.h"
#include <shlwapi.h>
#include <commdlg.h>

#pragma comment(lib, "shlwapi.lib")
#pragma comment(lib, "comdlg32.lib")

FileLoader::~FileLoader() {
    StopWatching();
}

std::string FileLoader::LoadFile(const std::wstring& path) {
    // FILE_SHARE_READ | FILE_SHARE_WRITE so we can read while editor has it open
    HANDLE hFile = CreateFileW(path.c_str(), GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hFile == INVALID_HANDLE_VALUE) return {};

    LARGE_INTEGER size;
    if (!GetFileSizeEx(hFile, &size) || size.QuadPart == 0) {
        CloseHandle(hFile);
        return {};
    }

    // Reject files larger than MAXDWORD (~4GB) to avoid ReadFile truncation
    if (size.QuadPart > static_cast<LONGLONG>(MAXDWORD)) {
        CloseHandle(hFile);
        return {};
    }

    std::string content(static_cast<size_t>(size.QuadPart), '\0');
    DWORD bytesRead = 0;
    BOOL ok = ReadFile(hFile, content.data(), static_cast<DWORD>(size.QuadPart), &bytesRead, nullptr);
    CloseHandle(hFile);

    if (!ok) return {};
    content.resize(bytesRead);

    // Strip UTF-8 BOM if present
    if (content.size() >= 3 &&
        static_cast<unsigned char>(content[0]) == 0xEF &&
        static_cast<unsigned char>(content[1]) == 0xBB &&
        static_cast<unsigned char>(content[2]) == 0xBF) {
        content.erase(0, 3);
    }

    return content;
}

std::wstring FileLoader::OpenFileDialog(HWND owner) {
    wchar_t filename[MAX_PATH] = {};
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = owner;
    ofn.lpstrFilter = L"Markdown Files\0*.md;*.markdown;*.mkd;*.txt\0All Files\0*.*\0";
    ofn.lpstrFile = filename;
    ofn.nMaxFile = MAX_PATH;
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
    ofn.lpstrDefExt = L"md";

    if (GetOpenFileNameW(&ofn)) {
        return filename;
    }
    return {};
}

static FILETIME GetFileWriteTime(const std::wstring& path) {
    WIN32_FILE_ATTRIBUTE_DATA attrs{};
    if (GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &attrs)) {
        return attrs.ftLastWriteTime;
    }
    return {};
}

void FileLoader::StartWatching(const std::wstring& file_path, ChangeCallback callback) {
    StopWatching();
    watch_path_ = file_path;
    on_change_ = std::move(callback);
    last_write_time_ = GetFileWriteTime(file_path);
    last_reload_tick_ = GetTickCount64();
    watching_ = true;
}

void FileLoader::StopWatching() {
    watching_ = false;
    on_change_ = nullptr;
}

void FileLoader::CheckForChanges() {
    if (!watching_) return;

    // Debounce: skip check if too soon after last reload
    ULONGLONG now = GetTickCount64();
    if (now - last_reload_tick_ < DEBOUNCE_MS) return;

    FILETIME current = GetFileWriteTime(watch_path_);

    // Check for zero FILETIME (file temporarily missing during atomic save)
    if (current.dwLowDateTime == 0 && current.dwHighDateTime == 0) return;

    if (CompareFileTime(&current, &last_write_time_) != 0) {
        last_write_time_ = current;
        last_reload_tick_ = now;
        if (on_change_) {
            on_change_();
        }
    }
}
