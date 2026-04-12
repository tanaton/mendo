#pragma once
#include <string>
#include <string_view>
#include <memory_resource>
#include <expected>
#include <windows.h>

// ファイル読み込みの最大サイズ（256MB）。FileLoader / ImageLoader で共有。
inline constexpr LONGLONG MAX_FILE_SIZE = 256LL * 1024 * 1024;

// FileLoader::LoadFile のエラー型。
enum class FileLoadError {
    NotFound,    // ファイルが見つからない、またはアクセス拒否
    TooLarge,    // ファイルサイズが MAX_FILE_SIZE を超過
    ReadFailed,  // 読み込み中のI/Oエラー
};

// FileLoadError に対応するローカライズ済みメッセージを返す。
// i18n.h に依存しないよう、呼び出し側が Strings を渡す設計。
inline std::wstring_view FileLoadErrorMessage(FileLoadError e, const auto& strings) noexcept
{
    switch (e) {
    case FileLoadError::NotFound:   return strings.toast_file_not_found;
    case FileLoadError::TooLarge:   return strings.toast_file_too_large;
    case FileLoadError::ReadFailed: return strings.toast_file_read_failed;
    default:                        return strings.toast_file_not_found;
    }
}

// ファイル読み込みユーティリティ（静的メソッドのみ）。
class FileLoader {
public:
    static std::expected<std::pmr::string, FileLoadError> LoadFile(const std::pmr::wstring& path);
    static std::pmr::wstring OpenFileDialog(HWND owner);
};
