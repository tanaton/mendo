#pragma once
#include <string>
#include <string_view>
#include <vector>
#include <windows.h>
#include <memory_resource>

struct FileEntry {
    std::pmr::wstring filename;
    std::pmr::wstring full_path;
    bool is_current = false;
    bool is_directory = false;
    bool is_parent = false;  // ".." entry
};

class FileExplorer {
public:
    void SetDirectory(std::wstring_view dir_path);
    void Refresh();
    const std::pmr::vector<FileEntry>& GetEntries() const noexcept { return entries_; }
    int HitTest(float local_y, float item_height) const noexcept;
    void SetCurrentFile(std::wstring_view path);
    const std::pmr::wstring& GetDirectory() const noexcept { return directory_; }

private:
    std::pmr::wstring directory_;
    std::pmr::vector<FileEntry> entries_;
};
