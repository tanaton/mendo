#pragma once
#include <string>
#include <vector>
#include <windows.h>

struct FileEntry {
    std::wstring filename;
    std::wstring full_path;
    bool is_current = false;
    bool is_directory = false;
    bool is_parent = false;  // ".." entry
};

class FileExplorer {
public:
    void SetDirectory(const std::wstring& dir_path);
    void Refresh();
    const std::vector<FileEntry>& GetEntries() const noexcept { return entries_; }
    int HitTest(float local_y, float item_height) const noexcept;
    void SetCurrentFile(const std::wstring& path);
    const std::wstring& GetDirectory() const noexcept { return directory_; }

private:
    std::wstring directory_;
    std::vector<FileEntry> entries_;
};
