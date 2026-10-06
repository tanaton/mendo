#include "file_dialog_service.h"
#include <commdlg.h>

#pragma comment(lib, "comdlg32.lib")

namespace {

using FileDialogFn = BOOL(WINAPI*)(LPOPENFILENAMEW);

std::pmr::wstring ShowFileDialog(FileDialogFn show, HWND owner, const wchar_t* filter, const wchar_t* default_ext,
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
        return std::pmr::wstring{ filename };
    }
    return {};
}

} // namespace

namespace file_dialog_service {

std::pmr::wstring OpenMarkdownFileDialog(HWND owner)
{
    return ShowFileDialog(GetOpenFileNameW, owner, L"Markdown Files\0*.md;*.markdown;*.mkd;*.txt\0All Files\0*.*\0", L"md",
                          OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST, nullptr);
}

std::pmr::wstring SavePngFileDialog(HWND owner, const wchar_t* default_filename)
{
    return ShowFileDialog(GetSaveFileNameW, owner, L"PNG Image\0*.png\0All Files\0*.*\0", L"png",
                          OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST, default_filename);
}

} // namespace file_dialog_service
