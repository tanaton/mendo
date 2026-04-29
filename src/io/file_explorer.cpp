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
    // ".." はソート対象外で常に先頭。後段ソートはこの範囲を除く。
    const size_t sort_begin = entries_.size();

    // ディレクトリ内の全アイテムを列挙
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
        // システムファイルをスキップ
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_SYSTEM) {
            continue;
        }
        // エントリ数上限
        if (entries_.size() - sort_begin >= MAX_ENTRIES) {
            break;
        }

        const bool is_dir = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
        if (!is_dir && !IsMarkdownFile(fd.cFileName)) {
            continue;
        }

        FileEntry entry;
        entry.full_path.assign((dir_base / fd.cFileName).native());
        entry.is_directory = is_dir;
        entries_.emplace_back(std::move(entry));
    } while (FindNextFileW(hFind.get(), &fd));

    // ディレクトリ優先 → 表示名 (大小無視) 昇順。
    // 比較対象は full_path 全体ではなく末尾ファイル名のみ（GetDisplayName）。
    std::ranges::sort(entries_.begin() + static_cast<ptrdiff_t>(sort_begin), entries_.end(),
        [](const FileEntry& a, const FileEntry& b) noexcept {
            if (a.is_directory != b.is_directory) {
                return a.is_directory > b.is_directory;
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
        entry.is_current = (!entry.is_directory && path_util::iequal(entry.full_path, path));
    }
}
