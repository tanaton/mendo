#pragma once
#include <cstdint>
#include <string>
#include <string_view>
#include <memory_resource>
#include <expected>

enum class FileLoadError : uint8_t {
    NotFound,   // ファイルが見つからない、またはアクセス拒否
    TooLarge,   // ファイルサイズが上限を超過
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
    }
    std::unreachable();
}

// LoadFile が返す UTF-8 ドキュメントテキスト (BOM 除去・LF 正規化済) + 元の UTF-8 バイト数 (BOM 込み)。
// byte_size はリロード時の二段階保存検出 (IsFileLargerThan) や AnalyzeReloadDiff の参照用。
struct LoadedFileDoc {
    std::pmr::string text;
    size_t byte_size = 0;
};

class FileLoader {
public:
    // ファイルを ReadFile で string バッファへ直接読み込み、UTF-8 BOM を除去して LF に正規化して返す。
    // Document とリロード diff はこの出力が LF-only であることを前提にする (issue #273)。
    // wstring 経由の二重変換 (UTF-8 → wstring → UTF-8) を行わないため巨大ファイルで高速。
    static std::expected<LoadedFileDoc, FileLoadError> LoadFile(const std::pmr::wstring& path);
};
