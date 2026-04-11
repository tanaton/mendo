#pragma once
#include "win_handle.h"
#include <cstdint>
#include <filesystem>
#include <memory>
#include <utility>

// ファイルを全て読み込む。失敗時は{nullptr, 0}を返す。
// out_errorが非nullの場合、CreateFileW失敗時のGetLastError()を格納する。
inline std::pair<std::unique_ptr<uint8_t[]>, size_t> ReadAllBytes(
    const std::filesystem::path& path, DWORD* out_error = nullptr)
{
    if (out_error) {
        *out_error = 0;
    }
    UniqueHandle hFile(CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
    if (!hFile) {
        if (out_error) {
            *out_error = GetLastError();
        }
        return {};
    }
    LARGE_INTEGER file_size;
    if (!GetFileSizeEx(hFile.get(), &file_size) || file_size.QuadPart <= 0) {
        return {};
    }
    const auto size = static_cast<size_t>(file_size.QuadPart);
    if (size > UINT32_MAX) {
        return {};
    }
    auto buf = std::make_unique_for_overwrite<uint8_t[]>(size);
    DWORD bytes_read = 0;
    if (!ReadFile(hFile.get(), buf.get(), static_cast<DWORD>(size), &bytes_read, nullptr) ||
        bytes_read != static_cast<DWORD>(size)) {
        return {};
    }
    return {std::move(buf), size};
}

// ファイルに全て書き込む。成功時はtrueを返す。
inline bool WriteAllBytes(const std::filesystem::path& path, const void* data, size_t size)
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
