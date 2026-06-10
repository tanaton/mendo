#include "file_loader.h"
#include "file_io.h"
#include "profiler.h"
#include <algorithm>
#include <cstring>
#include <shlwapi.h>
#include <string_view>

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

    // 先頭 3 バイトを覗いて UTF-8 BOM (EF BB BF) ならスキップ、なければ string 先頭へ転記する。
    // 一括読み + erase(0,3) と違い BOM ありファイルでも全体スケールの memmove を発生させない。
    char prefix[3];
    const DWORD prefix_want = static_cast<DWORD>(std::min<size_t>(r.size, sizeof(prefix)));
    DWORD prefix_read = 0;
    if (!ReadFile(r.handle.get(), prefix, prefix_want, &prefix_read, nullptr) || prefix_read != prefix_want) {
        return std::unexpected(FileLoadError::ReadFailed);
    }

    constexpr std::string_view kUtf8Bom = "\xEF\xBB\xBF";
    const bool has_bom = (prefix_read == kUtf8Bom.size() && std::string_view{ prefix, kUtf8Bom.size() } == kUtf8Bom);
    const size_t carry = has_bom ? 0u : prefix_read;
    const size_t remaining = r.size - prefix_read;
    const size_t out_size = carry + remaining;

    LoadedFileDoc result;
    result.byte_size = r.size;
    result.text.resize_and_overwrite(out_size, [&](char* buf, size_t /*cap*/) noexcept -> size_t {
        if (carry > 0) {
            std::memcpy(buf, prefix, carry);
        }
        if (remaining == 0) {
            return carry;
        }
        if (!ReadExact(r.handle.get(), buf + carry, remaining)) {
            return 0;
        }
        return carry + remaining;
    });
    if (result.text.size() != out_size) {
        return std::unexpected(FileLoadError::ReadFailed);
    }
    return result;
}
