#pragma once
#include <cstdint>
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

    // 戻り値は full_path の内部バッファを指す view。
    constexpr std::wstring_view GetDisplayName() const noexcept
    {
        if (is_parent()) {
            return L"..";
        }
        const std::wstring_view full{ full_path };
        const auto pos = full.find_last_of(L"\\/");
        return (pos != full.npos) ? full.substr(pos + 1) : full;
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

// 列挙は SetDirectory / Refresh 後の最初の GetEntries まで遅らせる。非表示のペインからは
// 呼ばれないため、起動時やファイルを開くたびの UI スレッド I/O が起きない。
class FileExplorer {
public:
    void SetDirectory(std::wstring_view dir_path);
    constexpr void Refresh() noexcept
    {
        stale_ = true;
    }
    const std::pmr::vector<FileEntry>& GetEntries() const;
    // 列挙し直すたびに進む。項目 index と組にすれば一覧中の項目を識別できる。
    constexpr uint32_t GetGeneration() const noexcept
    {
        return generation_;
    }
    int HitTest(float local_y, float item_height) const;
    // 以降の SetDirectory / Refresh でも強調表示が維持される。
    void SetCurrentFile(std::wstring_view path);
    constexpr const std::pmr::wstring& GetDirectory() const noexcept
    {
        return directory_;
    }

private:
    void ListEntries() const;
    void ApplyCurrentFile() const;

    std::pmr::wstring directory_;
    std::pmr::wstring current_file_;
    // GetEntries で遅延列挙するため mutable。
    mutable std::pmr::vector<FileEntry> entries_;
    mutable uint32_t generation_ = 0;
    mutable bool stale_ = false;
};
