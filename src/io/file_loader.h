#pragma once
#include <string>
#include <string_view>
#include <memory_resource>
#include <expected>
#include <windows.h>

// ファイル読み込みの最大サイズ（4GB）。
inline constexpr LONGLONG MAX_FILE_SIZE = 1024LL * 1024 * 1024 * 4;

// FileLoader::LoadFile のエラー型
enum class FileLoadError : uint8_t {
    NotFound,   // ファイルが見つからない、またはアクセス拒否
    TooLarge,   // ファイルサイズが MAX_FILE_SIZE を超過
    ReadFailed, // 読み込み中のI/Oエラー
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
    default:
        return strings.toast_file_not_found;
    }
}

// LoadFile が返す UTF-16 化済みドキュメントテキスト + 元の UTF-8 バイト数。
// byte_size はリロード時の二段階保存検出 (IsFileLargerThan) や AnalyzeReloadDiff の参照用。
struct LoadedFileWide {
    std::pmr::wstring wide;
    size_t byte_size = 0;
};

// ファイル読み込みユーティリティ（静的メソッドのみ）
class FileLoader {
public:
    // ファイルをメモリマップして UTF-8 → UTF-16 変換まで行い、結果のみ返す。
    // 中間 UTF-8 バッファを確保しないので、巨大ファイルでも UTF-8 のコピーが発生しない。
    static std::expected<LoadedFileWide, FileLoadError> LoadFile(const std::pmr::wstring& path);
    static std::pmr::wstring OpenFileDialog(HWND owner);
};
