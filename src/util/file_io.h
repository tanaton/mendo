#pragma once
#include "ascii_util.h"
#include "utility.h"
#include "win_handle.h"
#include <windows.h>
#include <algorithm>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <limits>
#include <memory_resource>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace path_util {

// 外部エディタとの同時アクセスを許容する共有モード。
// 編集中のファイルを mendo で開いたまま再読込できるようにするため、
// Markdown / 画像ファイルを扱う経路ではこれを指定する。
inline constexpr DWORD kFileShareRWDelete = FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE;

// ReadAllBytes が許容する最大サイズ。ReadFile は DWORD (32bit unsigned) で
// バイト数を扱うので、それを超えるサイズは 1 回の ReadFile で読み切れない。
inline constexpr size_t MAX_READABLE_FILE_SIZE = std::numeric_limits<uint32_t>::max();

// `CompareStringOrdinal(..., TRUE)` ベースで NTFS と挙動が一致する
// Unicode 込みの ordinal case-insensitive 比較。
inline bool iequal(std::wstring_view a, std::wstring_view b) noexcept
{
    return ::CompareStringOrdinal(a.data(), static_cast<int>(a.size()), b.data(), static_cast<int>(b.size()), TRUE) == CSTR_EQUAL;
}

inline bool iless(std::wstring_view a, std::wstring_view b) noexcept
{
    return ::CompareStringOrdinal(a.data(), static_cast<int>(a.size()), b.data(), static_cast<int>(b.size()), TRUE) == CSTR_LESS_THAN;
}

constexpr bool IsPathSlash(wchar_t c) noexcept
{
    return c == L'/' || c == L'\\';
}

// "C:\" のようなルートの区切りは残す。
constexpr std::wstring_view TrimTrailingSeparators(std::wstring_view path) noexcept
{
    while (path.size() > 3 && IsPathSlash(path.back())) {
        path.remove_suffix(1);
    }
    return path;
}

// \??\ は Win32 の正規化を通らず NT パスとして解釈されるので \??\UNC\host も届いてしまう。
constexpr bool HasNtPrefix(std::wstring_view path) noexcept
{
    return path.size() >= 4 && IsPathSlash(path[0]) && path[1] == L'?' && path[2] == L'?' && IsPathSlash(path[3]);
}

// 開いてもネットワークに出ないパスか。UNC (\\host・//host) やデバイスパス (\\?\・\\.\・\??\) は
// 開くだけで SMB/WebDAV 接続と NTLM 認証が走る。
constexpr bool IsLocalPath(std::wstring_view path) noexcept
{
    if (path.size() < 2 || !IsPathSlash(path[0])) {
        return true;
    }
    return !IsPathSlash(path[1]) && !HasNtPrefix(path);
}

// 最後のパス区切り以降。区切りで終わるパスでは空。
constexpr std::wstring_view FilenameView(std::wstring_view path) noexcept
{
    const auto pos = path.find_last_of(L"\\/");
    return pos == std::wstring_view::npos ? path : path.substr(pos + 1);
}

// 最後のパス区切り以降の "." を含む拡張子。無ければ空。
constexpr std::wstring_view ExtensionView(std::wstring_view path) noexcept
{
    const auto name = FilenameView(path);
    const auto dot_pos = name.rfind(L'.');
    return dot_pos == std::wstring_view::npos ? std::wstring_view{} : name.substr(dot_pos);
}

// 拡張子の大文字小文字は区別しない。
constexpr bool IsMarkdownFile(std::wstring_view path) noexcept
{
    constexpr ascii_util::LowercaseAsciiLiteral kMarkdownExts[]{
        L".md",
        L".markdown",
        L".mkd",
    };
    const auto ext = ExtensionView(path);
    return std::ranges::any_of(kMarkdownExts, [&](const auto& e) noexcept { return ascii_util::iequal(ext, e); });
}

// 空パスや区切りを含まないパスでは空を返す。
inline std::pmr::wstring ParentDirectory(std::wstring_view path)
{
    return std::pmr::wstring{ std::filesystem::path(path).parent_path().native() };
}

// C: か C:\ で始まるか。
constexpr bool StartsWithDrive(std::wstring_view path) noexcept
{
    return path.size() >= 2 && ascii_util::IsAsciiAlpha(path[0]) && path[1] == L':' && (path.size() == 2 || IsPathSlash(path[2]));
}

// \\?\C:\a・\\.\C:\a を C:\a に、\\?\UNC\host\share を \\host\share に直す。
// デバイス名前空間では .. がドライブや共有を越えて \\?\UNC\host へ届くが、Win32 形式ならルートで止まる。
// ドライブでも UNC でもないデバイスパス (\\.\pipe\x 等) は nullopt。それ以外のパスはそのまま返す。
inline std::optional<std::wstring> ToWin32Path(std::wstring_view path)
{
    const bool device = path.size() >= 4 && IsPathSlash(path[0]) && IsPathSlash(path[1]) && (path[2] == L'?' || path[2] == L'.') && IsPathSlash(path[3]);
    if (!device && !HasNtPrefix(path)) {
        return std::wstring(path);
    }
    const auto rest = path.substr(4);
    if (StartsWithDrive(rest)) {
        return std::wstring(rest);
    }
    if (rest.size() >= 4 && iequal(rest.substr(0, 3), L"UNC") && IsPathSlash(rest[3])) {
        return L"\\\\" + std::wstring(rest.substr(4));
    }
    return std::nullopt;
}

} // namespace path_util

enum class FileLoadError : uint8_t {
    NotFound,   // ファイルが見つからない、またはアクセス拒否 (CreateFileW の失敗)
    TooLarge,   // ファイルサイズが上限を超過
    ReadFailed, // サイズの取得や読み込み中のI/Oエラー
};

inline std::wstring_view FileLoadErrorMessage(FileLoadError e, const auto& strings) noexcept
{
    switch (e) {
    case FileLoadError::NotFound:
        return strings.toast_file_not_found;
    case FileLoadError::TooLarge:
        return strings.toast_file_too_large;
    case FileLoadError::ReadFailed:
        return strings.toast_file_read_failed;
    }
    std::unreachable();
}

struct OpenedFile {
    UniqueHandle handle;
    size_t size = 0;
};

// CreateFileW + GetFileSizeEx + サイズ上限チェックを束ねる。共有モードと最大サイズは呼び出し側で指定する。
// out_error は CreateFileW 失敗時のみ GetLastError() を格納する。
// 読み取り系は wstring から filesystem::path を作るヒープ確保を避けるため、NUL 終端文字列で受ける。
[[nodiscard]] std::expected<OpenedFile, FileLoadError> OpenFileForReadShared(const wchar_t* path, DWORD share_mode, LONGLONG max_size,
                                                                             DWORD* out_error = nullptr) noexcept;

// 失敗時と空ファイルは空を返す。out_error が非 null の場合、CreateFileW 失敗時の GetLastError() を格納する。
[[nodiscard]] ByteBuffer ReadAllBytes(const wchar_t* path, DWORD* out_error = nullptr,
                                    DWORD share_mode = FILE_SHARE_READ,
                                    LONGLONG max_size = static_cast<LONGLONG>(path_util::MAX_READABLE_FILE_SIZE));

// 指定バイト数を ReadFile の DWORD 上限でチャンク分割して読み切る。
// EOF・パイプ切断 (bytes_read == 0) は失敗として false を返す。
[[nodiscard]] inline bool ReadExact(HANDLE file, void* dst, size_t size) noexcept
{
    auto* const out = static_cast<uint8_t*>(dst);
    size_t total_read = 0;
    while (total_read < size) {
        DWORD bytes_read = 0;
        const DWORD to_read = static_cast<DWORD>(std::min<size_t>(size - total_read, std::numeric_limits<DWORD>::max()));
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
bool IsFileLargerThan(const wchar_t* path, size_t reference_size, size_t tolerance = 16) noexcept;

// flush_buffers: true の場合、close 前に FlushFileBuffers でデータブロックを
// ディスクへコミットする。rename と組み合わせて使う経路 (AtomicWriteAllBytes) 向け。
[[nodiscard]] bool WriteAllBytes(const std::filesystem::path& path, const void* data, size_t size, bool flush_buffers = false);

// tmp ファイルへ書いてから MoveFileExW(REPLACE_EXISTING) で原子的に置き換える。
// rename が失敗 (クロスボリューム等) した場合は tmp を削除し false を返す。直書きフォールバックは
// CREATE_ALWAYS が原本を切り詰めてから失敗しうるため行わない。
// tmp パスは `<path>.tmp` 固定 — 同じ path への並行呼び出しは tmp が衝突するため、
// 呼び出し側で排他制御すること (現 callsite は UI スレッド単一発火で安全)。
// 戻り値はアトミックな置換に成功したか。false の場合は原本が変更されていないことを保証する。
// durable=false は flush と write-through を省く。電源断で中身が壊れても読む側で検証して捨てられる
// キャッシュ向け (同期書き込みで終了が遅れないようにする)。
bool AtomicWriteAllBytes(const std::filesystem::path& path, const void* data, size_t size, bool durable = true);
