#include "file_explorer.h"
#include "file_io.h"
#include "document_utils.h"
#include "win_handle.h"
#include <algorithm>
#include <filesystem>
#include <iterator>

void FileExplorer::SetDirectory(std::wstring_view dir_path)
{
    std::pmr::wstring normalized{ dir_path };
    // ルートパスは除く
    while (normalized.size() > 3 && (normalized.back() == L'\\' || normalized.back() == L'/')) {
        normalized.pop_back();
    }
    if (directory_ == normalized) {
        return;
    }
    directory_ = std::move(normalized);
    Refresh();
}

void FileExplorer::Refresh()
{
    entries_.clear();
    if (directory_.empty()) {
        return;
    }

    // ルートでは ".." を出さない
    {
        const std::filesystem::path dir_path{ directory_ };
        const auto parent = dir_path.parent_path();
        if (parent != dir_path) {
            FileEntry pe;
            pe.full_path.assign(parent.native());
            pe.set_directory(true);
            pe.set_parent(true);
            entries_.emplace_back(std::move(pe));
        }
    }
    // ".." はソート対象外で常に先頭。後段ソートはこの範囲を除く。
    const size_t sort_begin = entries_.size();

    const std::filesystem::path dir_base{ directory_ };
    const auto pattern = dir_base / L"*";
    WIN32_FIND_DATAW fd;
    UniqueFindHandle hFind{ FindFirstFileW(pattern.c_str(), &fd) };
    if (!hFind) {
        return;
    }

    static constexpr size_t MAX_ENTRIES = 4096;

    do {
        const std::wstring_view name{ fd.cFileName };
        if (name == L"." || name == L"..") {
            continue;
        }
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_SYSTEM) {
            continue;
        }
        if (entries_.size() - sort_begin >= MAX_ENTRIES) {
            break;
        }

        const bool is_dir = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
        if (!is_dir && !IsMarkdownFile(fd.cFileName)) {
            continue;
        }

        FileEntry entry;
        entry.full_path.assign((dir_base / fd.cFileName).native());
        entry.set_directory(is_dir);
        entries_.emplace_back(std::move(entry));
    } while (FindNextFileW(hFind.get(), &fd));

    std::ranges::sort(entries_.begin() + static_cast<ptrdiff_t>(sort_begin), entries_.end(), [](const FileEntry& a, const FileEntry& b) noexcept {
        if (a.is_directory() != b.is_directory()) {
            return a.is_directory() > b.is_directory();
        }
        return path_util::iless(a.GetDisplayName(), b.GetDisplayName());
    });
}

int FileExplorer::HitTest(float local_y, float item_height) const noexcept
{
    if (local_y < 0 || item_height <= 0) {
        return -1;
    }
    const int index = static_cast<int>(local_y / item_height);
    if (index < 0 || index >= static_cast<int>(entries_.size())) {
        return -1;
    }
    return index;
}

void FileExplorer::SetCurrentFile(std::wstring_view path)
{
    for (auto& entry : entries_) {
        entry.set_current(!entry.is_directory() && path_util::iequal(entry.full_path, path));
    }
}
