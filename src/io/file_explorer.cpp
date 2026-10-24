#include "file_explorer.h"
#include "file_io.h"
#include "document_utils.h"
#include "win_handle.h"
#include <algorithm>
#include <filesystem>

namespace {
bool IsDirectory(const WIN32_FIND_DATAW& fd) noexcept
{
    return (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

// "."/".."・システム属性を除外し、ディレクトリまたは Markdown ファイルのみ一覧に含める。
bool ShouldListEntry(const WIN32_FIND_DATAW& fd) noexcept
{
    const std::wstring_view name{ fd.cFileName };
    if (name == L"." || name == L".." || (fd.dwFileAttributes & FILE_ATTRIBUTE_SYSTEM) != 0) {
        return false;
    }
    return IsDirectory(fd) || IsMarkdownFile(fd.cFileName);
}
} // namespace

std::wstring_view FileEntry::GetDisplayName() const noexcept
{
    return is_parent ? std::wstring_view{ L".." } : FilenameView(full_path);
}

void FileExplorer::SetDirectory(std::wstring_view dir_path)
{
    const std::wstring_view normalized = path_util::TrimTrailingSeparators(dir_path);
    if (directory_ == normalized) {
        return;
    }
    directory_ = normalized;
    // 前のフォルダで頼まれた表示を、ユーザーが移った先の一覧に持ち越さない。
    reveal_deferred_ = false;
    Refresh();
}

const std::pmr::vector<FileEntry>& FileExplorer::GetEntries() const
{
    if (stale_) {
        stale_ = false;
        ++request_;
        if (lister_ && !directory_.empty()) {
            // ネットワークドライブや大きいフォルダでは数百 ms かかるので、終わるまで前の一覧を見せておく。
            listing_ = true;
            lister_(directory_, request_);
        }
        else {
            ReplaceEntries(directory_.empty() ? std::pmr::vector<FileEntry>{} : List(directory_));
        }
    }
    return entries_;
}

bool FileExplorer::ApplyListing(uint32_t request, std::pmr::vector<FileEntry> entries)
{
    // 列挙し直しを待っている間 (フォルダの移動直後など) は、届いた結果も古い。
    if (request != request_ || !listing_ || stale_) {
        return false;
    }
    ReplaceEntries(std::move(entries));
    return true;
}

void FileExplorer::ReplaceEntries(std::pmr::vector<FileEntry> entries) const
{
    listing_ = false;
    ++generation_;
    entries_ = std::move(entries);
    ApplyCurrentFile();
}

std::pmr::vector<FileEntry> FileExplorer::List(std::wstring_view dir)
{
    std::pmr::vector<FileEntry> entries;
    const std::filesystem::path dir_base{ dir };
    // ルートでは ".." を出さない
    if (auto parent = dir_base.parent_path(); parent != dir_base) {
        FileEntry pe;
        pe.full_path.assign(parent.native());
        pe.is_directory = true;
        pe.is_parent = true;
        entries.emplace_back(std::move(pe));
    }
    // ".." はソート対象外で常に先頭。後段ソートはこの範囲を除く。
    const size_t sort_begin = entries.size();

    const auto pattern = dir_base / L"*";
    WIN32_FIND_DATAW fd;
    // 短縮名は使わないので取得を省き、大きなバッファで列挙してディレクトリの往復を減らす。
    UniqueFindHandle hFind{ FindFirstFileExW(pattern.c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch, nullptr, FIND_FIRST_EX_LARGE_FETCH) };
    if (!hFind) {
        return entries;
    }

    static constexpr size_t MAX_ENTRIES = 4096;

    for (;;) {
        if (ShouldListEntry(fd)) {
            FileEntry entry;
            entry.full_path.assign((dir_base / fd.cFileName).native());
            entry.is_directory = IsDirectory(fd);
            entries.emplace_back(std::move(entry));
        }
        if (entries.size() - sort_begin >= MAX_ENTRIES) {
            break;
        }
        if (!FindNextFileW(hFind.get(), &fd)) {
            // 列挙の途中失敗を正常終了 (ERROR_NO_MORE_FILES) と区別する。
            // 中断時は部分結果を完全な一覧と誤認させないよう破棄する。
            if (GetLastError() != ERROR_NO_MORE_FILES) {
                entries.resize(sort_begin);
            }
            break;
        }
    }

    std::ranges::sort(entries.begin() + static_cast<ptrdiff_t>(sort_begin), entries.end(), [](const FileEntry& a, const FileEntry& b) static noexcept {
        if (a.is_directory != b.is_directory) {
            return a.is_directory > b.is_directory;
        }
        return path_util::iless(a.GetDisplayName(), b.GetDisplayName());
    });
    return entries;
}

void FileExplorer::SetCurrentFile(std::wstring_view path)
{
    current_file_ = path;
    ApplyCurrentFile();
}

void FileExplorer::ApplyCurrentFile() const
{
    for (auto& entry : entries_) {
        entry.is_current = !entry.is_directory && path_util::iequal(entry.full_path, current_file_);
    }
}
