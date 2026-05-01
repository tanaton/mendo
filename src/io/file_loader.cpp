#include "file_loader.h"
#include "file_io.h"
#include "string_convert.h"
#include "win_handle.h"
#include <limits>
#include <shlwapi.h>
#include <commdlg.h>

#pragma comment(lib, "shlwapi.lib")
#pragma comment(lib, "comdlg32.lib")

std::expected<LoadedFileWide, FileLoadError> FileLoader::LoadFile(const std::pmr::wstring& path)
{
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
        return LoadedFileWide{};
    }

    // MultiByteToWideChar は cb*Char が int なので INT_MAX を超える入力を扱えない。
    // MAX_FILE_SIZE が int 上限を超える設定でも、ここで実効上限を強制し
    // Utf8ToWideStripBom が黙って空文字列を返す失敗モード (LoadFile としては成功扱いになる) を防ぐ。
    if (r.size > static_cast<size_t>(std::numeric_limits<int>::max())) {
        return std::unexpected(FileLoadError::TooLarge);
    }

    // メモリマップで UTF-8 を直接 view し、UTF-16 変換のヒープ確保のみに抑える。
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

    LoadedFileWide result;
    result.byte_size = r.size;
    string_convert::Utf8ToWideStripBom(bytes, result.wide);
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
