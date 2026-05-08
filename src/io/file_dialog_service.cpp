#include "file_dialog_service.h"
#include <commdlg.h>

#pragma comment(lib, "comdlg32.lib")

namespace file_dialog_service {

std::pmr::wstring OpenMarkdownFileDialog(HWND owner)
{
    wchar_t filename[MAX_PATH] = {};
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = owner;
    ofn.lpstrFilter = L"Markdown Files\0*.md;*.markdown;*.mkd;*.txt\0All Files\0*.*\0";
    ofn.lpstrFile = filename;
    ofn.nMaxFile = MAX_PATH;
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
    ofn.lpstrDefExt = L"md";

    if (GetOpenFileNameW(&ofn)) {
        return std::pmr::wstring{ filename };
    }
    return {};
}

} // namespace file_dialog_service
