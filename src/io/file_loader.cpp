#include "file_loader.h"
#include "file_io.h"
#include "win_handle.h"
#include <shlwapi.h>
#include <commdlg.h>

#pragma comment(lib, "shlwapi.lib")
#pragma comment(lib, "comdlg32.lib")

std::expected<std::pmr::string, FileLoadError> FileLoader::LoadFile(const std::pmr::wstring& path)
{
    // エディタがファイルを開いている間も読み取れるよう共有モードを許容
    auto r = OpenFileForReadShared(
        std::filesystem::path(path.c_str()),
        FILE_SHARE_RW_DELETE, MAX_FILE_SIZE);
    switch (r.error) {
    case OpenFileError::NotFound:
        return std::unexpected(FileLoadError::NotFound);
    case OpenFileError::SizeQueryFailed:
        return std::unexpected(FileLoadError::ReadFailed);
    case OpenFileError::TooLarge:
        return std::unexpected(FileLoadError::TooLarge);
    case OpenFileError::None:
        break;
    }

    if (r.size == 0) {
        return std::pmr::string{};
    }

    // BOM の除去は呼び出し側 (Utf8ToWideStripBom) が担うため、ここではバイト列をそのまま返す。
    std::pmr::string content;
    DWORD bytesRead = 0;
    BOOL ok = FALSE;
    content.resize_and_overwrite(r.size, [&](char* buf, size_t count) -> size_t {
        ok = ReadFile(r.handle.get(), buf, static_cast<DWORD>(count), &bytesRead, nullptr);
        return ok ? bytesRead : 0;
    });

    if (!ok) {
        return std::unexpected(FileLoadError::ReadFailed);
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
