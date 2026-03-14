#include "file_explorer.h"
#include <algorithm>

void FileExplorer::SetDirectory(const std::wstring& dir_path) {
    if (directory_ == dir_path) return;
    directory_ = dir_path;
    Refresh();
}

void FileExplorer::Refresh() {
    entries_.clear();
    if (directory_.empty()) return;

    std::wstring pattern = directory_ + L"\\*.md";
    WIN32_FIND_DATAW fd;
    HANDLE hFind = FindFirstFileW(pattern.c_str(), &fd);
    if (hFind == INVALID_HANDLE_VALUE) return;

    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;

        FileEntry entry;
        entry.filename = fd.cFileName;
        entry.full_path = directory_ + L"\\" + fd.cFileName;
        entries_.push_back(std::move(entry));
    } while (FindNextFileW(hFind, &fd));

    FindClose(hFind);

    // Sort alphabetically (case-insensitive)
    std::sort(entries_.begin(), entries_.end(), [](const FileEntry& a, const FileEntry& b) {
        return _wcsicmp(a.filename.c_str(), b.filename.c_str()) < 0;
    });
}

int FileExplorer::HitTest(float local_y, float item_height) const {
    if (local_y < 0 || item_height <= 0) return -1;
    int index = static_cast<int>(local_y / item_height);
    if (index < 0 || index >= static_cast<int>(entries_.size())) return -1;
    return index;
}

void FileExplorer::SetCurrentFile(const std::wstring& path) {
    for (auto& entry : entries_) {
        entry.is_current = (_wcsicmp(entry.full_path.c_str(), path.c_str()) == 0);
    }
}
