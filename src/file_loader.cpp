#include "file_loader.h"
#include <shlwapi.h>
#include <commdlg.h>

#pragma comment(lib, "shlwapi.lib")
#pragma comment(lib, "comdlg32.lib")

FileLoader::~FileLoader() {
    StopWatching();
}

std::pmr::string FileLoader::LoadFile(const std::pmr::wstring& path) {
    // エディタがファイルを開いている間も読み取れるよう FILE_SHARE_READ | FILE_SHARE_WRITE を指定
    HANDLE hFile = CreateFileW(path.c_str(), GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hFile == INVALID_HANDLE_VALUE) return {};

    LARGE_INTEGER size;
    if (!GetFileSizeEx(hFile, &size) || size.QuadPart == 0) {
        CloseHandle(hFile);
        return {};
    }

    // ReadFileの切り捨てを防ぐため、MAXDWORD（約4GB）を超えるファイルを拒否
    if (size.QuadPart > static_cast<LONGLONG>(MAXDWORD)) {
        CloseHandle(hFile);
        return {};
    }

    // UTF-8 BOMを先に検出し、全内容の memmove を回避する
    size_t file_size = static_cast<size_t>(size.QuadPart);
    size_t bom_skip = 0;
    if (file_size >= 3) {
        unsigned char bom[3]{};
        DWORD bom_read = 0;
        if (ReadFile(hFile, bom, 3, &bom_read, nullptr) && bom_read == 3 &&
            bom[0] == 0xEF && bom[1] == 0xBB && bom[2] == 0xBF) {
            bom_skip = 3;
        } else {
            // BOMなし: ファイル先頭に巻き戻す
            SetFilePointer(hFile, 0, nullptr, FILE_BEGIN);
        }
    }

    std::pmr::string content;
    DWORD bytesRead = 0;
    BOOL ok = FALSE;
    content.resize_and_overwrite(file_size - bom_skip, [&](char* buf, size_t count) -> size_t {
        ok = ReadFile(hFile, buf, static_cast<DWORD>(count), &bytesRead, nullptr);
        return ok ? bytesRead : 0;
    });
    CloseHandle(hFile);

    if (!ok) return {};

    return content;
}

std::pmr::wstring FileLoader::OpenFileDialog(HWND owner) {
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

static FILETIME GetFileWriteTime(const std::pmr::wstring& path) {
    WIN32_FILE_ATTRIBUTE_DATA attrs{};
    if (GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &attrs)) {
        return attrs.ftLastWriteTime;
    }
    return {};
}

void FileLoader::StartWatching(const std::pmr::wstring& file_path, ChangeCallback callback) {
    StopWatching();
    watch_path_ = file_path;
    on_change_ = std::move(callback);
    last_write_time_ = GetFileWriteTime(file_path);
    last_reload_tick_ = GetTickCount64();
    watching_ = true;
}

void FileLoader::StopWatching() noexcept {
    watching_ = false;
    on_change_ = nullptr;
}

void FileLoader::CheckForChanges() {
    if (!watching_) return;

    // デバウンス: 最後のリロードから間隔が短すぎる場合はチェックをスキップ
    ULONGLONG now = GetTickCount64();
    if (now - last_reload_tick_ < DEBOUNCE_MS) return;

    FILETIME current = GetFileWriteTime(watch_path_);

    // FILETIMEがゼロかチェック（アトミック保存中にファイルが一時的に消失する場合）
    if (current.dwLowDateTime == 0 && current.dwHighDateTime == 0) return;

    if (CompareFileTime(&current, &last_write_time_) != 0) {
        last_write_time_ = current;
        last_reload_tick_ = now;
        if (on_change_) {
            on_change_();
        }
    }
}
