#include "file_explorer.h"
#include <algorithm>

void FileExplorer::SetDirectory(std::wstring_view dir_path) {
    std::pmr::wstring normalized{dir_path};
    // 末尾の区切り文字を除去（"C:\" のようなルートパスは除く）
    while (normalized.size() > 3 && (normalized.back() == L'\\' || normalized.back() == L'/')) {
        normalized.pop_back();
    }
    if (directory_ == normalized) return;
    directory_ = std::move(normalized);
    Refresh();
}

void FileExplorer::Refresh() {
    entries_.clear();
    if (directory_.empty()) return;

    // 親ディレクトリエントリ ".." を追加（"C:\" のようなルートでは追加しない）
    {
        // 親を検索: 末尾のバックスラッシュを除去し、最後の区切り文字を探す
        std::wstring_view parent_view{directory_};
        // ドライブルート（例: "C:\"）では ".." を追加しない
        if (parent_view.size() > 3 || (parent_view.size() == 3 && parent_view[1] != L':')) {
            auto pos = parent_view.find_last_of(L"\\/");
            if (pos != std::wstring_view::npos && pos > 0) {
                // "C:\" だけでないことを確認
                std::wstring parent_dir{parent_view.substr(0, pos)};
                // "C:" → "C:\" に変換
                if (parent_dir.size() == 2 && parent_dir[1] == L':') {
                    parent_dir += L"\\";
                }
                FileEntry pe;
                pe.filename = L"..";
                pe.full_path = parent_dir;
                pe.is_directory = true;
                pe.is_parent = true;
                entries_.push_back(std::move(pe));
            }
        }
    }

    // ディレクトリ内の全アイテムを列挙
    std::wstring pattern{directory_};
    pattern += L"\\*";
    WIN32_FIND_DATAW fd;
    HANDLE hFind = FindFirstFileW(pattern.c_str(), &fd);
    if (hFind == INVALID_HANDLE_VALUE) return;

    std::pmr::vector<FileEntry> dirs;
    std::pmr::vector<FileEntry> files;

    do {
        // "." と ".." をスキップ
        if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0)
            continue;
        // 隠しファイル/システムファイルをスキップ
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_HIDDEN) continue;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_SYSTEM) continue;

        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            FileEntry entry;
            entry.filename = fd.cFileName;
            entry.full_path = directory_;
            entry.full_path += L"\\";
            entry.full_path += fd.cFileName;
            entry.is_directory = true;
            dirs.push_back(std::move(entry));
        } else {
            // Markdownファイル（.md, .markdown, .mkd）のみ表示
            std::wstring name = fd.cFileName;
            auto dot_pos = name.rfind(L'.');
            if (dot_pos != std::wstring::npos) {
                auto ext = name.substr(dot_pos);
                if (_wcsicmp(ext.c_str(), L".md") == 0 ||
                    _wcsicmp(ext.c_str(), L".markdown") == 0 ||
                    _wcsicmp(ext.c_str(), L".mkd") == 0) {
                    FileEntry entry;
                    entry.filename = fd.cFileName;
                    entry.full_path = directory_;
                    entry.full_path += L"\\";
                    entry.full_path += fd.cFileName;
                    files.push_back(std::move(entry));
                }
            }
        }
    } while (FindNextFileW(hFind, &fd));

    FindClose(hFind);

    // ディレクトリとファイルをそれぞれ大文字小文字無視でソート
    std::sort(dirs.begin(), dirs.end(), [](const FileEntry& a, const FileEntry& b) {
        return _wcsicmp(a.filename.c_str(), b.filename.c_str()) < 0;
    });
    std::sort(files.begin(), files.end(), [](const FileEntry& a, const FileEntry& b) {
        return _wcsicmp(a.filename.c_str(), b.filename.c_str()) < 0;
    });

    // 追加: ディレクトリを先に、次にファイル
    entries_.insert(entries_.end(), std::make_move_iterator(dirs.begin()),
                    std::make_move_iterator(dirs.end()));
    entries_.insert(entries_.end(), std::make_move_iterator(files.begin()),
                    std::make_move_iterator(files.end()));
}

int FileExplorer::HitTest(float local_y, float item_height) const noexcept {
    if (local_y < 0 || item_height <= 0) return -1;
    int index = static_cast<int>(local_y / item_height);
    if (index < 0 || index >= static_cast<int>(entries_.size())) return -1;
    return index;
}

void FileExplorer::SetCurrentFile(std::wstring_view path) {
    std::wstring path_str{path};
    for (auto& entry : entries_) {
        entry.is_current = (!entry.is_directory &&
                            _wcsicmp(entry.full_path.c_str(), path_str.c_str()) == 0);
    }
}
