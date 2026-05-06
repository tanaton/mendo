#pragma once
#include "doc_text.h"
#include <string>
#include <string_view>
#include <memory_resource>
#include <expected>
#include <windows.h>

// ファイル読み込みの最大サイズ（4GB）。Markdown 経路の実効上限は ~2GB-1。
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

// LoadFile が返す UTF-8 ドキュメントテキスト (BOM 除去済) + 元の UTF-8 バイト数 (BOM 込み)。
// byte_size はリロード時の二段階保存検出 (IsFileLargerThan) や AnalyzeReloadDiff の参照用。
struct LoadedFileDoc {
    std::pmr::string text;
    size_t byte_size = 0;
};

// ファイル読み込みユーティリティ（静的メソッドのみ）
class FileLoader {
public:
    // ファイルをメモリマップで読み、UTF-8 BOM を除去した string を返す。
    // wstring 経由の二重変換 (UTF-8 → wstring → UTF-8) を行わないため巨大ファイルで高速。
    static std::expected<LoadedFileDoc, FileLoadError> LoadFile(const std::pmr::wstring& path);
    static std::pmr::wstring OpenFileDialog(HWND owner);
};
