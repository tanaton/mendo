#pragma once
#include "win_handle.h"
#include <windows.h>
#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <memory>
#include <string_view>
#include <utility>

namespace path_util {

// 外部エディタとの同時アクセスを許容する共有モード。
// 編集中のファイルを mendo で開いたまま再読込できるようにするため、
// Markdown / 画像ファイルを扱う経路ではこれを指定する。
inline constexpr DWORD kFileShareRWDelete =
    FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE;

// ReadAllBytes が許容する最大サイズ。ReadFile は DWORD (32bit unsigned) で
// バイト数を扱うので、それを超えるサイズは 1 回の ReadFile で読み切れない。
inline constexpr size_t MAX_READABLE_FILE_SIZE = std::numeric_limits<uint32_t>::max();

} // namespace path_util

// OpenFileForReadShared の失敗区分。
enum class OpenFileError : uint8_t {
    None,            // 成功
    NotFound,        // CreateFileW 失敗（存在しない/アクセス拒否など）
    SizeQueryFailed, // GetFileSizeEx 失敗 or 負のサイズ
    TooLarge,        // max_size を超過
};

// CreateFileW + GetFileSizeEx + サイズ上限チェックを束ねた共通ヘルパーの結果。
// 失敗時は handle が空で、error に区分が入る。
struct OpenedFile {
    UniqueHandle handle;
    size_t size = 0;
    OpenFileError error = OpenFileError::None;
};

// 共有モードと最大サイズは呼び出し側で指定する。
// out_error は CreateFileW 失敗時のみ GetLastError() を格納する。
[[nodiscard]] OpenedFile OpenFileForReadShared(const std::filesystem::path& path, DWORD share_mode, LONGLONG max_size, DWORD* out_error = nullptr) noexcept;

// out_error が非 null の場合、CreateFileW 失敗時の GetLastError() を格納する。
[[nodiscard]] std::pair<std::unique_ptr<uint8_t[]>, size_t> ReadAllBytes(
    const std::filesystem::path& path, DWORD* out_error = nullptr);

// 指定バイト数を ReadFile の DWORD 上限でチャンク分割して読み切る。
// EOF・パイプ切断 (bytes_read == 0) は失敗として false を返す。
[[nodiscard]] inline bool ReadExact(HANDLE file, void* dst, size_t size) noexcept
{
    auto* const out = static_cast<uint8_t*>(dst);
    size_t total_read = 0;
    while (total_read < size) {
        DWORD bytes_read = 0;
        const DWORD to_read = static_cast<DWORD>(std::min<size_t>(size - total_read, UINT32_MAX));
        if (!ReadFile(file, out + total_read, to_read, &bytes_read, nullptr) || bytes_read == 0) {
            return false;
        }
        total_read += bytes_read;
    }
    return true;
}

// 読み込み済みコンテンツの後にファイルがさらに伸びていれば、エディタ側が
// 書き込み途中である可能性が高い。BOM の 3 バイトずれ等を吸収するため
// 16 バイトの許容範囲を持たせる。
bool IsFileLargerThan(const std::filesystem::path& path, size_t reference_size, size_t tolerance = 16) noexcept;

[[nodiscard]] bool WriteAllBytes(const std::filesystem::path& path, const void* data, size_t size);

// tmp ファイルへ書いてから MoveFileExW(REPLACE_EXISTING) で原子的に置き換える。
// rename が失敗 (クロスボリューム等) した場合は tmp を削除し false を返す。直書きフォールバックは
// CREATE_ALWAYS が原本を切り詰めてから失敗しうるため行わない。
// tmp パスは `<path>.tmp` 固定 — 同じ path への並行呼び出しは tmp が衝突するため、
// 呼び出し側で排他制御すること (現 callsite は UI スレッド単一発火で安全)。
// 戻り値はアトミックな置換に成功したか。false の場合は原本が変更されていないことを保証する。
bool AtomicWriteAllBytes(const std::filesystem::path& path, const void* data, size_t size);

// ファイル名・フルパス比較ユーティリティ。
// `CompareStringOrdinal(..., TRUE)` ベースで NTFS と挙動が一致する
// Unicode 込みの ordinal case-insensitive 比較を提供する。
namespace path_util {

inline bool iequal(std::wstring_view a, std::wstring_view b) noexcept
{
    return ::CompareStringOrdinal(a.data(), static_cast<int>(a.size()), b.data(), static_cast<int>(b.size()), TRUE) == CSTR_EQUAL;
}

inline bool iless(std::wstring_view a, std::wstring_view b) noexcept
{
    return ::CompareStringOrdinal(a.data(), static_cast<int>(a.size()), b.data(), static_cast<int>(b.size()), TRUE) == CSTR_LESS_THAN;
}

} // namespace path_util
