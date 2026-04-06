#pragma once
#include <string>
#include <memory_resource>
#include <windows.h>

// ファイル読み込みの最大サイズ（256MB）。FileLoader / ImageLoader で共有。
inline constexpr LONGLONG MAX_FILE_SIZE = 256LL * 1024 * 1024;

// ファイル読み込みユーティリティ（静的メソッドのみ）。
class FileLoader {
public:
    static std::pmr::string LoadFile(const std::pmr::wstring& path);
    static std::pmr::wstring OpenFileDialog(HWND owner);
};
