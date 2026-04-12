#include "file_explorer.h"
#include "document_utils.h"
#include "win_handle.h"
#include <algorithm>
#include <filesystem>
#include <iterator>

void FileExplorer::SetDirectory(std::wstring_view dir_path)
{
    std::pmr::wstring normalized{ dir_path };
    // 末尾の区切り文字を除去（"C:\" のようなルートパスは除く）
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

    // 親ディレクトリエントリ ".." を追加（"C:\" のようなルートでは追加しない）
    {
        const std::filesystem::path dir_path{ directory_ };
        const auto parent = dir_path.parent_path();
        if (parent != dir_path) {
            FileEntry pe;
            pe.full_path.assign(parent.native());
            pe.is_directory = true;
            pe.is_parent = true;
            entries_.emplace_back(std::move(pe));
        }
    }

    // ディレクトリ内の全アイテムを列挙
    const std::filesystem::path dir_base{ directory_ };
    const auto pattern = dir_base / L"*";
    WIN32_FIND_DATAW fd;
    UniqueFindHandle hFind{ FindFirstFileW(pattern.c_str(), &fd) };
    if (!hFind) {
        return;
    }

    std::pmr::vector<FileEntry> dirs;
    std::pmr::vector<FileEntry> files;
    static constexpr size_t MAX_ENTRIES = 4096;

    do {
        // "." と ".." をスキップ
        if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0) {
            continue;
        }
        // システムファイルをスキップ
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_SYSTEM) {
            continue;
        }
        // エントリ数上限
        if (dirs.size() + files.size() >= MAX_ENTRIES) {
            break;
        }

        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            FileEntry entry;
            entry.full_path.assign((dir_base / fd.cFileName).native());
            entry.is_directory = true;
            dirs.emplace_back(std::move(entry));
        }
        else if (IsMarkdownFile(fd.cFileName)) {
            FileEntry entry;
            entry.full_path.assign((dir_base / fd.cFileName).native());
            files.emplace_back(std::move(entry));
        }
    } while (FindNextFileW(hFind.get(), &fd));

    // ディレクトリとファイルをそれぞれ大文字小文字無視でソート
    std::ranges::sort(dirs, [](const FileEntry& a, const FileEntry& b) static noexcept {
        return _wcsicmp(a.GetDisplayName(), b.GetDisplayName()) < 0;
    });
    std::ranges::sort(files, [](const FileEntry& a, const FileEntry& b) static noexcept {
        return _wcsicmp(a.GetDisplayName(), b.GetDisplayName()) < 0;
    });

    // 追加: ディレクトリを先に、次にファイル
    entries_.reserve(entries_.size() + dirs.size() + files.size());
    std::ranges::move(dirs, std::back_inserter(entries_));
    std::ranges::move(files, std::back_inserter(entries_));
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
    const std::pmr::wstring path_str{ path };
    for (auto& entry : entries_) {
        entry.is_current = (!entry.is_directory && _wcsicmp(entry.full_path.c_str(), path_str.c_str()) == 0);
    }
}
