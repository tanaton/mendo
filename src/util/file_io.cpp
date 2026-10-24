#include "file_io.h"
#include <new>

OpenedFile OpenFileForReadShared(const wchar_t* path, DWORD share_mode, LONGLONG max_size, DWORD* out_error) noexcept
{
    if (out_error) {
        *out_error = 0;
    }
    OpenedFile r;
    UniqueHandle hFile(CreateFileW(path, GENERIC_READ, share_mode, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr));
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

std::pmr::vector<uint8_t> ReadAllBytes(const wchar_t* path, DWORD* out_error, DWORD share_mode, LONGLONG max_size)
{
    auto r = OpenFileForReadShared(path, share_mode, max_size, out_error);
    if (r.error != OpenFileError::None || r.size == 0) {
        return {};
    }
    // worker からも呼ぶので、巨大ファイルで確保に失敗しても例外を外へ出さず失敗扱いにする。
    std::pmr::vector<uint8_t> buf;
    try {
        buf.resize(r.size);
    } catch (const std::bad_alloc&) {
        return {};
    }
    if (!ReadExact(r.handle.get(), buf.data(), r.size)) {
        return {};
    }
    return buf;
}

namespace {

// 取得できなければ nullopt (存在しない・アクセス不可・仮想パス)。
std::optional<uint64_t> QueryFileSize(const wchar_t* path) noexcept
{
    WIN32_FILE_ATTRIBUTE_DATA attr{};
    if (!GetFileAttributesExW(path, GetFileExInfoStandard, &attr)) {
        return std::nullopt;
    }
    return (static_cast<uint64_t>(attr.nFileSizeHigh) << 32) | static_cast<uint64_t>(attr.nFileSizeLow);
}

} // namespace

bool IsFileLargerThan(const wchar_t* path, size_t reference_size, size_t tolerance) noexcept
{
    const auto size = QueryFileSize(path);
    return size && *size > static_cast<uint64_t>(reference_size) + tolerance;
}

bool WriteAllBytes(const std::filesystem::path& path, const void* data, size_t size, bool flush_buffers)
{
    if (size > std::numeric_limits<DWORD>::max()) {
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

bool AtomicWriteAllBytes(const std::filesystem::path& path, const void* data, size_t size, bool durable)
{
    std::filesystem::path tmp_path = path;
    tmp_path += L".tmp";

    // tmp のデータブロックを flush してから write-through rename する。
    // rename のメタデータだけが先にディスクへコミットされると、電源断時に
    // 対象ファイルが 0 バイト化し旧内容が復元不能になるため。
    if (!WriteAllBytes(tmp_path, data, size, /* flush_buffers = */ durable)) {
        // 書き込み途中失敗でも CREATE_ALWAYS で tmp は生成済み。部分書き込みの
        // 孤児 tmp を残さないよう掃除する。
        DeleteFileW(tmp_path.c_str());
        return false;
    }

    const DWORD move_flags = MOVEFILE_REPLACE_EXISTING | (durable ? MOVEFILE_WRITE_THROUGH : 0);
    if (MoveFileExW(tmp_path.c_str(), path.c_str(), move_flags)) {
        return true;
    }

    // 直書きフォールバックは CREATE_ALWAYS が原本を切り詰めてから失敗しうるため行わない。
    DeleteFileW(tmp_path.c_str());
    return false;
}
