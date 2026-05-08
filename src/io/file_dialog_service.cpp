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

std::pmr::wstring SavePngFileDialog(HWND owner, const wchar_t* default_filename)
{
    wchar_t filename[MAX_PATH] = {};
    if (default_filename) {
        // wcsncpy_s を使うと VC++ 固有依存が増えるので size_t で長さを clamp してコピー。
        size_t i = 0;
        for (; i + 1 < MAX_PATH && default_filename[i]; ++i) {
            filename[i] = default_filename[i];
        }
        filename[i] = L'\0';
    }
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = owner;
    ofn.lpstrFilter = L"PNG Image\0*.png\0All Files\0*.*\0";
    ofn.lpstrFile = filename;
    ofn.nMaxFile = MAX_PATH;
    ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST;
    ofn.lpstrDefExt = L"png";

    if (GetSaveFileNameW(&ofn)) {
        return std::pmr::wstring{ filename };
    }
    return {};
}

} // namespace file_dialog_service
