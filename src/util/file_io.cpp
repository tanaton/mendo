#include "file_io.h"
#include <commdlg.h>
#include <format>
#include <new>

std::expected<OpenedFile, FileLoadError> OpenFileForReadShared(const wchar_t* path, LONGLONG max_size) noexcept
{
    UniqueHandle hFile(CreateFileW(path, GENERIC_READ, path_util::kFileShareRWDelete, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr));
    if (!hFile) {
        const DWORD error = GetLastError();
        return std::unexpected(error == ERROR_SHARING_VIOLATION || error == ERROR_LOCK_VIOLATION ? FileLoadError::Locked : FileLoadError::NotFound);
    }
    LARGE_INTEGER file_size;
    if (!GetFileSizeEx(hFile.get(), &file_size) || file_size.QuadPart < 0) {
        return std::unexpected(FileLoadError::ReadFailed);
    }
    if (file_size.QuadPart > max_size) {
        return std::unexpected(FileLoadError::TooLarge);
    }
    return OpenedFile{ std::move(hFile), static_cast<size_t>(file_size.QuadPart) };
}

ByteBuffer ReadAllBytes(const wchar_t* path, LONGLONG max_size)
{
    auto r = OpenFileForReadShared(path, max_size);
    if (!r || r->size == 0) {
        return {};
    }
    // worker からも呼ぶので、巨大ファイルで確保に失敗しても例外を外へ出さず失敗扱いにする。
    ByteBuffer buf;
    try {
        buf = ByteBuffer(r->size);
    } catch (const std::bad_alloc&) {
        return {};
    }
    if (!ReadExact(r->handle.get(), buf.data(), r->size)) {
        return {};
    }
    return buf;
}

bool IsFileLargerThan(const wchar_t* path, size_t reference_size) noexcept
{
    constexpr uint64_t kTolerance = 16;
    // 大きさを取れない (存在しない・アクセス不可・仮想パス) なら大きくない。
    WIN32_FILE_ATTRIBUTE_DATA attr{};
    if (!GetFileAttributesExW(path, GetFileExInfoStandard, &attr)) {
        return false;
    }
    const uint64_t size = (static_cast<uint64_t>(attr.nFileSizeHigh) << 32) | static_cast<uint64_t>(attr.nFileSizeLow);
    return size > static_cast<uint64_t>(reference_size) + kTolerance;
}

bool AtomicWriteUserFile(const std::filesystem::path& path, const void* data, size_t size)
{
    if (size > std::numeric_limits<DWORD>::max()) {
        return false;
    }
    constexpr DWORD kMaxAttempts = 16;
    std::filesystem::path tmp_path;
    UniqueHandle hFile;
    for (DWORD attempt = 0; attempt < kMaxAttempts && !hFile; ++attempt) {
        tmp_path = path;
        tmp_path += std::format(L".{}-{}.tmp", GetCurrentProcessId(), attempt);
        // CREATE_NEW なので、同じ名前のファイルがあれば触らずに次の名前を試す。
        hFile.reset(CreateFileW(tmp_path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr));
        if (!hFile && GetLastError() != ERROR_FILE_EXISTS) {
            return false;
        }
    }
    if (!hFile) {
        return false;
    }
    // データブロックを FlushFileBuffers でディスクへコミットしてから write-through で rename する。rename の
    // メタデータだけが先にディスクへコミットされると、電源断で対象ファイルが 0 バイトになり旧内容を戻せない。
    DWORD bytes_written = 0;
    const bool written = WriteFile(hFile.get(), data, static_cast<DWORD>(size), &bytes_written, nullptr) &&
                         bytes_written == static_cast<DWORD>(size) && FlushFileBuffers(hFile.get());
    hFile.reset();
    if (written && MoveFileExW(tmp_path.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        return true;
    }
    DeleteFileW(tmp_path.c_str());
    return false;
}

namespace {

using FileDialogFn = BOOL(WINAPI*)(LPOPENFILENAMEW);

std::wstring ShowFileDialog(FileDialogFn show, HWND owner, const wchar_t* filter, const wchar_t* default_ext,
                            DWORD flags, const wchar_t* initial_name)
{
    wchar_t filename[MAX_PATH] = {};
    if (initial_name) {
        // lstrcpynW は MAX_PATH 内で常に NUL 終端を保証する Win32 API。
        lstrcpynW(filename, initial_name, MAX_PATH);
    }
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = owner;
    ofn.lpstrFilter = filter;
    ofn.lpstrFile = filename;
    ofn.nMaxFile = MAX_PATH;
    ofn.Flags = flags;
    ofn.lpstrDefExt = default_ext;

    if (show(&ofn)) {
        return std::wstring{ filename };
    }
    return {};
}

} // namespace

namespace file_dialog {

std::wstring OpenMarkdownFile(HWND owner)
{
    return ShowFileDialog(GetOpenFileNameW, owner, L"Markdown Files\0*.md;*.markdown;*.mkd;*.txt\0All Files\0*.*\0", L"md",
                          OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST, nullptr);
}

std::wstring SavePngFile(HWND owner, const wchar_t* default_filename)
{
    return ShowFileDialog(GetSaveFileNameW, owner, L"PNG Image\0*.png\0All Files\0*.*\0", L"png",
                          OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST, default_filename);
}

} // namespace file_dialog
