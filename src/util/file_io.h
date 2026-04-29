#pragma once
#include "win_handle.h"
#include <windows.h>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string_view>
#include <utility>

// 外部エディタとの同時アクセスを許容する共有モード。
// 編集中のファイルを mendo で開いたまま再読込できるようにするため、
// Markdown / 画像ファイルを扱う経路ではこれを指定する。
inline constexpr DWORD FILE_SHARE_RW_DELETE =
    FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE;

// OpenFileForReadShared の失敗区分。
enum class OpenFileError : uint8_t {
    None,             // 成功
    NotFound,         // CreateFileW 失敗（存在しない/アクセス拒否など）
    SizeQueryFailed,  // GetFileSizeEx 失敗 or 負のサイズ
    TooLarge,         // max_size を超過
};

// CreateFileW + GetFileSizeEx + サイズ上限チェックを束ねた共通ヘルパー。
// 共有モードと最大サイズは呼び出し側で指定する。
// 失敗時は handle が空で、error に区分が入る。
// out_error は CreateFileW 失敗時のみ GetLastError() を格納する
// （SizeQueryFailed / TooLarge では更新しない）。
struct OpenedFile {
    UniqueHandle handle;
    size_t size = 0;
    OpenFileError error = OpenFileError::None;
};

[[nodiscard]] inline OpenedFile OpenFileForReadShared(const std::filesystem::path& path,
    DWORD share_mode, LONGLONG max_size, DWORD* out_error = nullptr) noexcept
{
    if (out_error) {
        *out_error = 0;
    }
    OpenedFile r;
    UniqueHandle hFile(CreateFileW(path.c_str(), GENERIC_READ, share_mode, nullptr,
        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
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

// ファイルを全て読み込む。失敗時は{nullptr, 0}を返す。
// out_errorが非nullの場合、CreateFileW失敗時のGetLastError()を格納する。
[[nodiscard]] inline std::pair<std::unique_ptr<uint8_t[]>, size_t> ReadAllBytes(
    const std::filesystem::path& path, DWORD* out_error = nullptr)
{
    auto r = OpenFileForReadShared(path, FILE_SHARE_READ, UINT32_MAX, out_error);
    if (r.error != OpenFileError::None || r.size == 0) {
        return {};
    }
    auto buf = std::make_unique_for_overwrite<uint8_t[]>(r.size);
    DWORD bytes_read = 0;
    if (!ReadFile(r.handle.get(), buf.get(), static_cast<DWORD>(r.size), &bytes_read, nullptr) ||
        bytes_read != static_cast<DWORD>(r.size)) {
        return {};
    }
    return { std::move(buf), r.size };
}

// 読み込み済みコンテンツの後にファイルがさらに伸びていれば、エディタ側が
// 書き込み途中である可能性が高い。BOM の 3 バイトずれ等を吸収するため
// 16 バイトの許容範囲を持たせる。
inline bool IsFileLargerThan(const std::filesystem::path& path,
    size_t reference_size, size_t tolerance = 16) noexcept
{
    WIN32_FILE_ATTRIBUTE_DATA attr{};
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &attr)) {
        return false;
    }
    const uint64_t current_size = (static_cast<uint64_t>(attr.nFileSizeHigh) << 32)
        | static_cast<uint64_t>(attr.nFileSizeLow);
    return current_size > static_cast<uint64_t>(reference_size) + tolerance;
}

// ファイルに全て書き込む。成功時はtrueを返す。
[[nodiscard]] inline bool WriteAllBytes(const std::filesystem::path& path, const void* data, size_t size)
{
    if (size > UINT32_MAX) {
        return false;
    }
    UniqueHandle hFile(CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr,
        CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr));
    if (!hFile) {
        return false;
    }
    DWORD bytes_written = 0;
    if (!WriteFile(hFile.get(), data, static_cast<DWORD>(size), &bytes_written, nullptr) ||
        bytes_written != static_cast<DWORD>(size)) {
        return false;
    }
    return true;
}

// ファイル名・フルパス比較ユーティリティ。
// `CompareStringOrdinal(..., TRUE)` ベースで NTFS と挙動が一致する
// Unicode 込みの ordinal case-insensitive 比較を提供する。
// 拡張子や URL スキーム等の ASCII 確定トークンには ascii_util::iequal を使う。
namespace path_util {

inline bool iequal(std::wstring_view a, std::wstring_view b) noexcept
{
    return ::CompareStringOrdinal(
        a.data(), static_cast<int>(a.size()),
        b.data(), static_cast<int>(b.size()),
        TRUE) == CSTR_EQUAL;
}

inline bool iless(std::wstring_view a, std::wstring_view b) noexcept
{
    return ::CompareStringOrdinal(
        a.data(), static_cast<int>(a.size()),
        b.data(), static_cast<int>(b.size()),
        TRUE) == CSTR_LESS_THAN;
}

} // namespace path_util
