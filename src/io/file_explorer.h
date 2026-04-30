#pragma once
#include <string>
#include <string_view>
#include <vector>
#include <memory_resource>

struct FileEntry {
    std::pmr::wstring full_path;

    constexpr bool is_current() const noexcept
    {
        return flags_ & CURRENT;
    }
    constexpr bool is_directory() const noexcept
    {
        return flags_ & DIRECTORY;
    }
    constexpr bool is_parent() const noexcept
    {
        return flags_ & PARENT;
    }

    constexpr void set_current(bool v) noexcept
    {
        set_flag(CURRENT, v);
    }
    constexpr void set_directory(bool v) noexcept
    {
        set_flag(DIRECTORY, v);
    }
    constexpr void set_parent(bool v) noexcept
    {
        set_flag(PARENT, v);
    }

    // 表示名を返す（full_pathの末尾ファイル名、".."エントリは"..") 。
    // 戻り値はfull_pathの内部バッファまたはリテラルを指すためnull終端。
    constexpr const wchar_t* GetDisplayName() const noexcept
    {
        if (is_parent()) {
            return L"..";
        }
        const auto pos = full_path.find_last_of(L"\\/");
        return (pos != full_path.npos) ? full_path.c_str() + pos + 1 : full_path.c_str();
    }

private:
    static constexpr uint8_t CURRENT = 0x01;
    static constexpr uint8_t DIRECTORY = 0x02;
    static constexpr uint8_t PARENT = 0x04;

    constexpr void set_flag(uint8_t mask, bool v) noexcept
    {
        flags_ = v ? (flags_ | mask) : static_cast<uint8_t>(flags_ & ~mask);
    }

    uint8_t flags_ = 0;
};

class FileExplorer {
public:
    void SetDirectory(std::wstring_view dir_path);
    void Refresh();
    constexpr const std::pmr::vector<FileEntry>& GetEntries() const noexcept
    {
        return entries_;
    }
    int HitTest(float local_y, float item_height) const noexcept;
    void SetCurrentFile(std::wstring_view path);
    constexpr const std::pmr::wstring& GetDirectory() const noexcept
    {
        return directory_;
    }

private:
    std::pmr::wstring directory_;
    std::pmr::vector<FileEntry> entries_;
};
