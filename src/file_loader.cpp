#include "file_loader.h"
#include <shlwapi.h>
#include <commdlg.h>
#include <filesystem>

#pragma comment(lib, "shlwapi.lib")
#pragma comment(lib, "comdlg32.lib")

FileLoader::~FileLoader()
{
    StopWatching();
}

std::pmr::string FileLoader::LoadFile(const std::pmr::wstring& path)
{
    // エディタがファイルを開いている間も読み取れるよう FILE_SHARE_READ | FILE_SHARE_WRITE を指定
    HANDLE hFile = CreateFileW(path.c_str(), GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hFile == INVALID_HANDLE_VALUE) {
        return {};
    }

    LARGE_INTEGER size;
    if (!GetFileSizeEx(hFile, &size) || size.QuadPart == 0) {
        CloseHandle(hFile);
        return {};
    }

    if (size.QuadPart > MAX_FILE_SIZE) {
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
        }
        else {
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

    if (!ok) {
        return {};
    }

    return content;
}

std::pmr::wstring FileLoader::OpenFileDialog(HWND owner)
{
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

void FileLoader::StartWatching(const std::pmr::wstring& file_path, ChangeCallback callback)
{
    StopWatching();
    watch_path_ = file_path;
    on_change_ = std::move(callback);
    last_reload_tick_ = GetTickCount64();

    // ファイル名部分を抽出
    std::filesystem::path p(std::wstring_view{ file_path });
    watch_filename_ = std::pmr::wstring{ std::wstring_view{ p.filename().native() } };

    // 親ディレクトリをReadDirectoryChangesWで監視
    auto dir = p.parent_path();
    if (dir.empty()) {
        return;
    }

    dir_handle_ = CreateFileW(
        dir.c_str(),
        FILE_LIST_DIRECTORY,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr,
        OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED,
        nullptr);

    if (dir_handle_ == INVALID_HANDLE_VALUE) {
        return;
    }

    overlapped_ = {};
    overlapped_.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!overlapped_.hEvent) {
        CloseHandle(dir_handle_);
        dir_handle_ = INVALID_HANDLE_VALUE;
        return;
    }

    watching_ = true;
    BeginRead();
}

void FileLoader::BeginRead()
{
    if (dir_handle_ == INVALID_HANDLE_VALUE) {
        return;
    }

    ResetEvent(overlapped_.hEvent);
    read_pending_ = ReadDirectoryChangesW(
        dir_handle_,
        change_buf_,
        sizeof(change_buf_),
        FALSE,
        FILE_NOTIFY_CHANGE_LAST_WRITE | FILE_NOTIFY_CHANGE_SIZE | FILE_NOTIFY_CHANGE_FILE_NAME,
        nullptr,
        &overlapped_,
        nullptr);
}

void FileLoader::StopWatching() noexcept
{
    if (read_pending_ && dir_handle_ != INVALID_HANDLE_VALUE) {
        CancelIo(dir_handle_);
        read_pending_ = false;
    }
    if (overlapped_.hEvent) {
        CloseHandle(overlapped_.hEvent);
        overlapped_ = {};
    }
    if (dir_handle_ != INVALID_HANDLE_VALUE) {
        CloseHandle(dir_handle_);
        dir_handle_ = INVALID_HANDLE_VALUE;
    }
    watching_ = false;
    on_change_ = nullptr;
}

void FileLoader::CheckForChanges()
{
    if (!watching_ || !read_pending_ || dir_handle_ == INVALID_HANDLE_VALUE) {
        return;
    }

    // 非ブロッキングで完了を確認
    DWORD bytes_returned = 0;
    if (!GetOverlappedResult(dir_handle_, &overlapped_, &bytes_returned, FALSE)) {
        return; // まだ完了していない
    }

    read_pending_ = false;

    // デバウンス: 最後のリロードから間隔が短すぎる場合はスキップ
    ULONGLONG now = GetTickCount64();
    bool debounced = (now - last_reload_tick_ < DEBOUNCE_MS);

    // 変更通知を解析し、監視対象のファイルが含まれているか確認
    bool target_changed = false;
    if (bytes_returned > 0 && !debounced) {
        auto* info = reinterpret_cast<FILE_NOTIFY_INFORMATION*>(change_buf_);
        for (;;) {
            std::wstring_view changed_name(info->FileName, info->FileNameLength / sizeof(wchar_t));
            if (changed_name.size() == watch_filename_.size() &&
                _wcsnicmp(changed_name.data(), watch_filename_.c_str(), changed_name.size()) == 0) {
                target_changed = true;
                break;
            }
            if (info->NextEntryOffset == 0) {
                break;
            }
            info = reinterpret_cast<FILE_NOTIFY_INFORMATION*>(
                reinterpret_cast<char*>(info) + info->NextEntryOffset);
        }
    }

    // 次の監視を開始
    BeginRead();

    if (target_changed) {
        last_reload_tick_ = now;
        if (on_change_) {
            on_change_();
        }
    }
}

void FileLoader::ResetDebounceTick() noexcept
{
    last_reload_tick_ = GetTickCount64();
}
