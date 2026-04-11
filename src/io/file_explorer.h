#pragma once
#include <string>
#include <string_view>
#include <vector>
#include <windows.h>
#include <memory_resource>

struct FileEntry {
    std::pmr::wstring full_path;
    bool is_current = false;
    bool is_directory = false;
    bool is_parent = false;  // ".." エントリ

    // 表示名を返す（full_pathの末尾ファイル名、".."エントリは"..") 。
    // 戻り値はfull_pathの内部バッファまたはリテラルを指すためnull終端。
    const wchar_t* GetDisplayName() const noexcept
    {
        if (is_parent) {
            return L"..";
        }
        const auto pos = full_path.find_last_of(L"\\/");
        return (pos != full_path.npos) ? full_path.c_str() + pos + 1 : full_path.c_str();
    }
};

class FileExplorer {
public:
    void SetDirectory(std::wstring_view dir_path);
    void Refresh();
    constexpr const std::pmr::vector<FileEntry>& GetEntries() const noexcept { return entries_; }
    int HitTest(float local_y, float item_height) const noexcept;
    void SetCurrentFile(std::wstring_view path);
    constexpr const std::pmr::wstring& GetDirectory() const noexcept { return directory_; }

private:
    std::pmr::wstring directory_;
    std::pmr::vector<FileEntry> entries_;
};
