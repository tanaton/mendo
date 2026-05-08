#include "file_loader.h"
#include "file_io.h"
#include "string_convert.h"
#include "win_handle.h"
#include <shlwapi.h>

#pragma comment(lib, "shlwapi.lib")

std::expected<LoadedFileDoc, FileLoadError> FileLoader::LoadFile(const std::pmr::wstring& path)
{
    MENDO_PROFILE("FileLoader::LoadFile");
    // エディタがファイルを開いている間も読み取れるよう共有モードを許容。
    auto r = OpenFileForReadShared(
        std::filesystem::path(path.c_str()),
        path_util::kFileShareRWDelete, MAX_FILE_SIZE);
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

