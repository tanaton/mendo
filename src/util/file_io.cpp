#include "file_io.h"
#include <limits>
#include <utility>

OpenedFile OpenFileForReadShared(const std::filesystem::path& path, DWORD share_mode, LONGLONG max_size, DWORD* out_error) noexcept
{
    if (out_error) {
        *out_error = 0;
    }
    OpenedFile r;
    UniqueHandle hFile(CreateFileW(path.c_str(), GENERIC_READ, share_mode, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr));
    if (!hFile) {
        if (out_error) {
            *out_error = GetLastError();
        }
        r.error = OpenFileError::NotFound;
        return r;
    }
    LARGE_INTEGER file_size;
    if (!GetFileSizeEx(hFile.get(), &file_size) || file_size.QuadPart < 0) {
        r.error = OpenFileError::SizeQueryFailed;
        return r;
    }
    if (file_size.QuadPart > max_size) {
        r.error = OpenFileError::TooLarge;
        return r;
    }
    r.handle = std::move(hFile);
    r.size = static_cast<size_t>(file_size.QuadPart);
    return r;
}

std::pair<std::unique_ptr<uint8_t[]>, size_t> ReadAllBytes(const std::filesystem::path& path, DWORD* out_error)
{
    auto r = OpenFileForReadShared(path, FILE_SHARE_READ, path_util::MAX_READABLE_FILE_SIZE, out_error);
    if (r.error != OpenFileError::None || r.size == 0) {
        return {};
    }
    auto buf = std::make_unique_for_overwrite<uint8_t[]>(r.size);
    if (!ReadExact(r.handle.get(), buf.get(), r.size)) {
        return {};
    }
    return { std::move(buf), r.size };
}

bool IsFileLargerThan(const std::filesystem::path& path, size_t reference_size, size_t tolerance) noexcept
{
    WIN32_FILE_ATTRIBUTE_DATA attr{};
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &attr)) {
        return false;
    }
    const uint64_t current_size = (static_cast<uint64_t>(attr.nFileSizeHigh) << 32) | static_cast<uint64_t>(attr.nFileSizeLow);
    return current_size > static_cast<uint64_t>(reference_size) + tolerance;
}

bool WriteAllBytes(const std::filesystem::path& path, const void* data, size_t size, bool flush_buffers)
{
    if (size > std::numeric_limits<uint32_t>::max()) {
        return false;
    }
    UniqueHandle hFile(CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr));
    if (!hFile) {
        return false;
    }
    DWORD bytes_written = 0;
    if (!WriteFile(hFile.get(), data, static_cast<DWORD>(size), &bytes_written, nullptr) ||
        bytes_written != static_cast<DWORD>(size)) {
        return false;
    }
    if (flush_buffers && !FlushFileBuffers(hFile.get())) {
        return false;
    }
    return true;
}

bool AtomicWriteAllBytes(const std::filesystem::path& path, const void* data, size_t size)
{
    std::filesystem::path tmp_path = path;
    tmp_path += L".tmp";

    // tmp のデータブロックを flush してから write-through rename する。
    // rename のメタデータだけが先にディスクへコミットされると、電源断時に
    // 対象ファイルが 0 バイト化し旧内容が復元不能になるため。
    if (!WriteAllBytes(tmp_path, data, size, /* flush_buffers = */ true)) {
        // 書き込み途中失敗でも CREATE_ALWAYS で tmp は生成済み。部分書き込みの
        // 孤児 tmp を残さないよう掃除する。
        DeleteFileW(tmp_path.c_str());
        return false;
    }

    if (MoveFileExW(tmp_path.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        return true;
    }

    // 直書きフォールバックは CREATE_ALWAYS が原本を切り詰めてから失敗しうるため行わない。
    DeleteFileW(tmp_path.c_str());
    return false;
}
