#include "file_loader.h"
#include "file_io.h"
#include "string_convert.h"
#include "win_handle.h"
#include <limits>
#include <shlwapi.h>
#include <commdlg.h>

#pragma comment(lib, "shlwapi.lib")
#pragma comment(lib, "comdlg32.lib")

std::expected<LoadedFileDoc, FileLoadError> FileLoader::LoadFile(const std::pmr::wstring& path)
{
    MENDO_PROFILE("FileLoader::LoadFile");
    // エディタがファイルを開いている間も読み取れるよう共有モードを許容。
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
        return LoadedFileDoc{};
    }

    // 下流の md4c / view_length は uint32_t を取り扱うため、INT_MAX 超えのファイルは弾く。
    if (r.size > static_cast<size_t>(std::numeric_limits<int>::max())) {
        return std::unexpected(FileLoadError::TooLarge);
    }

    // メモリマップで UTF-8 を直接 view し、BOM 除去後にそのまま string にコピーする。
    // OS のページキャッシュに乗ったまま読めるため、同じファイルの再オープンで物理 I/O が増えない。
    UniqueFileMapping hMapping(CreateFileMappingW(r.handle.get(), nullptr, PAGE_READONLY, 0, 0, nullptr));
    if (!hMapping) {
        return std::unexpected(FileLoadError::ReadFailed);
    }

    UniqueMapView view(MapViewOfFile(hMapping.get(), FILE_MAP_READ, 0, 0, 0));
    if (!view) {
        return std::unexpected(FileLoadError::ReadFailed);
    }

    const std::string_view bytes{ static_cast<const char*>(view.get()), r.size };

    LoadedFileDoc result;
    result.byte_size = r.size;
    result.text.assign(string_convert::StripUtf8Bom(bytes));
    return result;
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
