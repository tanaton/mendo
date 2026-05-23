#pragma once
#include "doc_text.h"
#include <string>
#include <string_view>
#include <memory_resource>
#include <expected>
#include <limits>
#include <windows.h>

// ファイル読み込みの最大サイズ。Markdown 経路は md4c が int を取るため ~2GB-1 が
// 実効上限。事前ガード (OpenFileForReadShared) もこの値で揃え、二段ガードを排除する。
inline constexpr LONGLONG MAX_FILE_SIZE = static_cast<LONGLONG>(std::numeric_limits<int>::max());

// FileLoader::LoadFile のエラー型
enum class FileLoadError : uint8_t {
    NotFound,   // ファイルが見つからない、またはアクセス拒否
    TooLarge,   // ファイルサイズが MAX_FILE_SIZE を超過
    ReadFailed, // 読み込み中のI/Oエラー
    Cancelled,  // 協調キャンセル (stop_token 経由)。UI にはトースト表示しない契約。
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
    case FileLoadError::Cancelled:
        return {};
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

// ファイル読み込みユーティリティ（静的メソッドのみ）。
// path 選択は file_dialog_service.h に分離してある。
class FileLoader {
public:
    // ファイルを ReadFile で string バッファへ直接読み込み、UTF-8 BOM を除去して返す。
    // wstring 経由の二重変換 (UTF-8 → wstring → UTF-8) を行わないため巨大ファイルで高速。
    static std::expected<LoadedFileDoc, FileLoadError> LoadFile(const std::pmr::wstring& path);
};
