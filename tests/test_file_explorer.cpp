#include <gtest/gtest.h>
#include <string>
#include <string_view>
#include <vector>
#include "file_explorer.h"
#include "test_helpers.h"
#include <algorithm>
#include <filesystem>

namespace fs = std::filesystem;

class FileExplorerTest : public TempDirTestBase {
protected:
    void CreateDir(const std::wstring& name)
    {
        fs::create_directories(temp_dir_ / name);
    }

    // 列挙を始めてから (lister が無ければその場で列挙して) 今の一覧を返す。
    const std::vector<FileEntry>& Entries()
    {
        explorer_.EnsureListed();
        return explorer_.GetEntries();
    }

    const std::vector<FileEntry>& ListTempDir()
    {
        explorer_.SetDirectory(temp_dir_.wstring());
        return Entries();
    }

    // ".." を除いた表示名を並び順のまま返す。
    std::vector<std::wstring> ListedNames()
    {
        std::vector<std::wstring> names;
        for (const auto& e : Entries()) {
            if (!e.is_parent) {
                names.emplace_back(e.GetDisplayName());
            }
        }
        return names;
    }

    const FileEntry* FindEntry(std::wstring_view name)
    {
        const auto& entries = Entries();
        const auto it = std::ranges::find(entries, name, &FileEntry::GetDisplayName);
        return it != entries.end() ? &*it : nullptr;
    }

    size_t CountCurrent()
    {
        return static_cast<size_t>(std::ranges::count_if(Entries(), &FileEntry::is_current));
    }

    FileExplorer explorer_;
};

// ---- 現在ファイルの強調表示 ----

TEST_F(FileExplorerTest, CurrentFileSurvivesRefreshAndDirectoryChange)
{
    WriteTempFile(L"a.md");
    CreateDir(L"sub");

    explorer_.SetCurrentFile((temp_dir_ / L"a.md").wstring());
    ListTempDir();
    EXPECT_EQ(CountCurrent(), 1u);

    explorer_.SetDirectory((temp_dir_ / L"sub").wstring());
    EXPECT_EQ(CountCurrent(), 0u);

    ListTempDir();
    EXPECT_EQ(CountCurrent(), 1u);

    explorer_.Refresh();
    EXPECT_EQ(CountCurrent(), 1u);
}

// ---- 基本的な列挙 ----

TEST_F(FileExplorerTest, EmptyDirectoryHasParentOnly)
{
    const auto& entries = ListTempDir();
    ASSERT_EQ(entries.size(), 1u);
    EXPECT_TRUE(entries[0].is_parent);
    EXPECT_EQ(entries[0].GetDisplayName(), L"..");
}

// ---- 遅延列挙 ----

TEST_F(FileExplorerTest, SetDirectoryDefersListingUntilEnsureListed)
{
    explorer_.SetCurrentFile((temp_dir_ / L"a.md").wstring());
    explorer_.SetDirectory(temp_dir_.wstring());
    WriteTempFile(L"a.md");

    ASSERT_EQ(Entries().size(), 2u);
    EXPECT_TRUE(Entries()[1].is_current);
}

// 列挙を別スレッドに任せると、結果が届くまで前の一覧のまま待つ。古い要求の結果は捨てる。
TEST_F(FileExplorerTest, AsyncListerKeepsPreviousEntriesUntilApplied)
{
    WriteTempFile(L"a.md");
    EXPECT_EQ(ListTempDir().size(), 2u);
    const uint32_t generation = explorer_.GetGeneration();

    std::vector<std::pair<std::wstring, uint32_t>> requests;
    explorer_.SetAsyncLister([&requests](std::wstring dir, uint32_t request) {
        requests.emplace_back(std::wstring{ dir }, request);
    });
    WriteTempFile(L"b.md");
    explorer_.Refresh();
    EXPECT_EQ(Entries().size(), 2u) << "届くまで前の一覧を見せる";
    Entries();
    ASSERT_EQ(requests.size(), 1u) << "待っている間は列挙し直さない";
    EXPECT_TRUE(explorer_.IsListing());

    explorer_.Refresh();
    Entries();
    ASSERT_EQ(requests.size(), 2u);
    EXPECT_FALSE(explorer_.ApplyListing(requests[0].second, FileExplorer::List(requests[0].first)));

    EXPECT_TRUE(explorer_.ApplyListing(requests[1].second, FileExplorer::List(requests[1].first)));
    EXPECT_FALSE(explorer_.IsListing());
    EXPECT_EQ(Entries().size(), 3u);
    EXPECT_NE(explorer_.GetGeneration(), generation);
}

// 列挙を待つ間に別のフォルダへ移ると、前のフォルダの結果と保留していた表示を捨てる。
TEST_F(FileExplorerTest, AsyncListerDropsResultAndRevealAfterDirectoryChange)
{
    CreateDir(L"sub");
    std::vector<std::pair<std::wstring, uint32_t>> requests;
    explorer_.SetAsyncLister([&requests](std::wstring dir, uint32_t request) {
        requests.emplace_back(std::wstring{ dir }, request);
    });
    explorer_.SetDirectory(temp_dir_.wstring());
    Entries();
    ASSERT_EQ(requests.size(), 1u);
    explorer_.DeferReveal();

    explorer_.SetDirectory((temp_dir_ / L"sub").wstring());
    EXPECT_FALSE(explorer_.ApplyListing(requests[0].second, FileExplorer::List(requests[0].first))) << "移動先を列挙し直す前に届いた前のフォルダの結果";
    EXPECT_FALSE(explorer_.TakeDeferredReveal());

    Entries();
    ASSERT_EQ(requests.size(), 2u);
    EXPECT_TRUE(explorer_.ApplyListing(requests[1].second, FileExplorer::List(requests[1].first)));
}

TEST_F(FileExplorerTest, ListedOnceUntilRefresh)
{
    EXPECT_EQ(ListTempDir().size(), 1u);
    const uint32_t generation = explorer_.GetGeneration();

    WriteTempFile(L"new.md");
    EXPECT_EQ(Entries().size(), 1u);
    EXPECT_EQ(explorer_.GetGeneration(), generation);

    explorer_.Refresh();
    EXPECT_EQ(Entries().size(), 2u);
    EXPECT_NE(explorer_.GetGeneration(), generation);
}

// ---- Bug #23: パスの正規化（末尾のバックスラッシュ） ----

// 末尾の区切りだけが違う同じディレクトリは列挙し直さず、full_path に二重の区切りを作らない。
TEST_F(FileExplorerTest, TrailingBackslashNormalized)
{
    WriteTempFile(L"hello.md");

    explorer_.SetDirectory(temp_dir_.wstring() + L"\\");
    EXPECT_EQ(std::wstring_view{ explorer_.GetDirectory() }, temp_dir_.wstring());
    const auto* entry = FindEntry(L"hello.md");
    ASSERT_NE(entry, nullptr);
    EXPECT_FALSE(entry->full_path.contains(L"\\\\")) << "full_pathに二重バックスラッシュあり";

    const uint32_t generation = explorer_.GetGeneration();
    explorer_.SetDirectory(temp_dir_.wstring());
    EXPECT_EQ(Entries().size(), 2u);
    EXPECT_EQ(explorer_.GetGeneration(), generation);
}

// .md / .markdown / .mkd (大文字小文字無視) とディレクトリだけを並べる。
TEST_F(FileExplorerTest, ListsMarkdownFilesAndDirectoriesOnly)
{
    for (const auto* name : { L"readme.md", L"doc.markdown", L"doc.mkd", L"upper.MD", L"image.png", L"data.json", L"script.py" }) {
        WriteTempFile(name);
    }
    CreateDir(L"subdir");

    ListTempDir();
    EXPECT_EQ(ListedNames(), (std::vector<std::wstring>{ L"subdir", L"doc.markdown", L"doc.mkd", L"readme.md", L"upper.MD" }));
}

TEST_F(FileExplorerTest, DirectoriesBeforeFiles)
{
    WriteTempFile(L"aaa.md");
    CreateDir(L"zzz_dir");

    const auto& entries = ListTempDir();
    ASSERT_EQ(entries.size(), 3u);
    EXPECT_TRUE(entries[0].is_parent);
    EXPECT_TRUE(entries[1].is_directory);
    EXPECT_FALSE(entries[2].is_directory);
    EXPECT_EQ(ListedNames(), (std::vector<std::wstring>{ L"zzz_dir", L"aaa.md" }));
}

TEST_F(FileExplorerTest, EntriesSortedCaseInsensitive)
{
    WriteTempFile(L"Bbb.md");
    WriteTempFile(L"aaa.md");
    WriteTempFile(L"ccc.md");

    ListTempDir();
    EXPECT_EQ(ListedNames(), (std::vector<std::wstring>{ L"aaa.md", L"Bbb.md", L"ccc.md" }));
}

// ---- SetCurrentFile テスト ----

TEST_F(FileExplorerTest, SetCurrentFileMarksEntry)
{
    WriteTempFile(L"a.md");
    WriteTempFile(L"b.md");

    ListTempDir();
    explorer_.SetCurrentFile((temp_dir_ / L"b.md").wstring());

    const auto* entry = FindEntry(L"b.md");
    ASSERT_NE(entry, nullptr);
    EXPECT_TRUE(entry->is_current);
    EXPECT_EQ(CountCurrent(), 1u);
}

TEST_F(FileExplorerTest, SetCurrentFileDoesNotMarkDirectories)
{
    CreateDir(L"subdir");

    ListTempDir();
    // パスが一致してもディレクトリはcurrentとしてマークされないこと
    explorer_.SetCurrentFile((temp_dir_ / L"subdir").wstring());
    EXPECT_EQ(CountCurrent(), 0u);
}

// ---- 初回起動シナリオ（ファイル未選択でディレクトリ表示） ----

TEST_F(FileExplorerTest, SetDirectoryWithoutCurrentFileHasNoCurrent)
{
    WriteTempFile(L"a.md");
    WriteTempFile(L"b.md");

    ListTempDir();
    EXPECT_EQ(CountCurrent(), 0u);
}

TEST_F(FileExplorerTest, SetDirectoryWithCurrentWorkingDirectory)
{
    // 実際のカレントディレクトリを使用するシナリオ（初回起動を模倣）
    wchar_t cwd[MAX_PATH];
    ASSERT_NE(GetCurrentDirectoryW(MAX_PATH, cwd), 0u);

    explorer_.SetDirectory(cwd);

    EXPECT_FALSE(explorer_.GetDirectory().empty());
    // 少なくとも ".." エントリがあること
    ASSERT_GE(Entries().size(), 1u);
    EXPECT_TRUE(Entries()[0].is_parent);
}

TEST_F(FileExplorerTest, SetDirectoryThenSetDirectoryAgainSwitches)
{
    // 初回起動でcwdを表示した後、ファイルのあるディレクトリに切り替えるシナリオ
    WriteTempFile(L"test.md");
    CreateDir(L"other");
    WriteTempFile(L"other/other.md");
    const fs::path other_dir = temp_dir_ / L"other";

    ListTempDir();
    EXPECT_EQ(std::wstring_view{ explorer_.GetDirectory() }, temp_dir_.wstring());

    explorer_.SetDirectory(other_dir.wstring());
    EXPECT_EQ(std::wstring_view{ explorer_.GetDirectory() }, other_dir.wstring());
    EXPECT_EQ(ListedNames(), (std::vector<std::wstring>{ L"other.md" }));
}

// ---- リフレッシュ / SetDirectory ----

TEST_F(FileExplorerTest, RefreshPicksUpNewFiles)
{
    const size_t before = ListTempDir().size();

    WriteTempFile(L"new.md");
    explorer_.Refresh();

    EXPECT_EQ(Entries().size(), before + 1);
}

TEST_F(FileExplorerTest, FullPathIsCorrect)
{
    WriteTempFile(L"test.md");

    ListTempDir();
    const auto* entry = FindEntry(L"test.md");
    ASSERT_NE(entry, nullptr);
    EXPECT_EQ(std::wstring_view{ entry->full_path }, temp_dir_.wstring() + L"\\test.md");
}
