#include <gtest/gtest.h>
#include <memory_resource>
#include <string_view>
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
};

// ---- 現在ファイルの強調表示 ----

TEST_F(FileExplorerTest, CurrentFileSurvivesRefreshAndDirectoryChange)
{
    WriteTempFile(L"a.md");
    CreateDir(L"sub");
    const auto current = (temp_dir_ / L"a.md").wstring();

    FileExplorer explorer;
    explorer.SetCurrentFile(current);
    explorer.SetDirectory(temp_dir_.wstring());
    const auto is_current = [](const FileEntry& e) { return e.is_current(); };
    EXPECT_EQ(std::ranges::count_if(explorer.GetEntries(), is_current), 1);

    explorer.SetDirectory((temp_dir_ / L"sub").wstring());
    EXPECT_EQ(std::ranges::count_if(explorer.GetEntries(), is_current), 0);

    explorer.SetDirectory(temp_dir_.wstring());
    EXPECT_EQ(std::ranges::count_if(explorer.GetEntries(), is_current), 1);

    explorer.Refresh();
    EXPECT_EQ(std::ranges::count_if(explorer.GetEntries(), is_current), 1);
}

// ---- 基本的な列挙 ----

TEST_F(FileExplorerTest, EmptyDirectoryHasParentOnly)
{
    FileExplorer explorer;
    explorer.SetDirectory(temp_dir_.wstring());
    // ".." 親エントリのみ
    auto& entries = explorer.GetEntries();
    ASSERT_EQ(entries.size(), 1u);
    EXPECT_TRUE(entries[0].is_parent());
    EXPECT_EQ(std::wstring_view(entries[0].GetDisplayName()), L"..");
}

// ---- 遅延列挙 ----

TEST_F(FileExplorerTest, SetDirectoryDefersListingUntilGetEntries)
{
    const auto current = (temp_dir_ / L"a.md").wstring();

    FileExplorer explorer;
    explorer.SetCurrentFile(current);
    explorer.SetDirectory(temp_dir_.wstring());
    WriteTempFile(L"a.md");

    ASSERT_EQ(explorer.GetEntries().size(), 2u);
    EXPECT_TRUE(explorer.GetEntries()[1].is_current());
}

TEST_F(FileExplorerTest, ListedOnceUntilRefresh)
{
    FileExplorer explorer;
    explorer.SetDirectory(temp_dir_.wstring());
    EXPECT_EQ(explorer.GetEntries().size(), 1u);
    const uint32_t generation = explorer.GetGeneration();

    WriteTempFile(L"new.md");
    EXPECT_EQ(explorer.GetEntries().size(), 1u);
    EXPECT_EQ(explorer.GetGeneration(), generation);

    explorer.Refresh();
    EXPECT_EQ(explorer.GetEntries().size(), 2u);
    EXPECT_NE(explorer.GetGeneration(), generation);
}

// ---- Bug #23: パスの正規化（末尾のバックスラッシュ） ----

TEST_F(FileExplorerTest, TrailingBackslashNormalized)
{
    WriteTempFile(L"test.md");
    FileExplorer explorer;

    std::wstring with_slash = temp_dir_.wstring() + L"\\";
    std::wstring without_slash = temp_dir_.wstring();

    explorer.SetDirectory(with_slash);
    size_t count1 = explorer.GetEntries().size();

    // 末尾スラッシュが異なる同じディレクトリを設定しても再リフレッシュしないこと
    // （正規化が機能していれば、directory_の比較で同じディレクトリと検出される）
    explorer.SetDirectory(without_slash);
    size_t count2 = explorer.GetEntries().size();

    EXPECT_EQ(count1, count2);
}

TEST_F(FileExplorerTest, TrailingSlashDoesNotCreateDoubleBackslash)
{
    WriteTempFile(L"hello.md");
    FileExplorer explorer;

    std::wstring with_slash = temp_dir_.wstring() + L"\\";
    explorer.SetDirectory(with_slash);

    // .mdファイルが問題なく見つかること
    bool found_md = false;
    for (const auto& entry : explorer.GetEntries()) {
        if (std::wstring_view(entry.GetDisplayName()) == L"hello.md") {
            found_md = true;
            // full_pathに二重バックスラッシュが含まれないこと
            EXPECT_EQ(entry.full_path.find(L"\\\\"), std::wstring::npos)
                << "full_pathに二重バックスラッシュが含まれています: "
                << "full_pathに二重バックスラッシュあり";
        }
    }
    EXPECT_TRUE(found_md);
}

TEST_F(FileExplorerTest, ShowsMdFiles)
{
    WriteTempFile(L"readme.md");
    WriteTempFile(L"notes.md");

    FileExplorer explorer;
    explorer.SetDirectory(temp_dir_.wstring());
    auto& entries = explorer.GetEntries();

    // ".." + mdファイル2つ
    EXPECT_EQ(entries.size(), 3u);
}

TEST_F(FileExplorerTest, ShowsMarkdownExtension)
{
    WriteTempFile(L"doc.markdown");

    FileExplorer explorer;
    explorer.SetDirectory(temp_dir_.wstring());
    auto& entries = explorer.GetEntries();

    // ".." + markdownファイル1つ
    EXPECT_EQ(entries.size(), 2u);
    EXPECT_EQ(std::wstring_view(entries[1].GetDisplayName()), L"doc.markdown");
}

TEST_F(FileExplorerTest, ShowsMkdExtension)
{
    WriteTempFile(L"doc.mkd");

    FileExplorer explorer;
    explorer.SetDirectory(temp_dir_.wstring());
    auto& entries = explorer.GetEntries();

    EXPECT_EQ(entries.size(), 2u);
    EXPECT_EQ(std::wstring_view(entries[1].GetDisplayName()), L"doc.mkd");
}

TEST_F(FileExplorerTest, HidesNonMarkdownFiles)
{
    WriteTempFile(L"readme.md");
    WriteTempFile(L"image.png");
    WriteTempFile(L"data.json");
    WriteTempFile(L"script.py");

    FileExplorer explorer;
    explorer.SetDirectory(temp_dir_.wstring());
    auto& entries = explorer.GetEntries();

    // ".." + mdファイル1つ（非mdファイルは非表示）
    EXPECT_EQ(entries.size(), 2u);
}

TEST_F(FileExplorerTest, ShowsDirectories)
{
    CreateDir(L"subdir");
    WriteTempFile(L"test.md");

    FileExplorer explorer;
    explorer.SetDirectory(temp_dir_.wstring());
    auto& entries = explorer.GetEntries();

    // ".." + ディレクトリ1つ + ファイル1つ
    EXPECT_EQ(entries.size(), 3u);
}

TEST_F(FileExplorerTest, DirectoriesBeforeFiles)
{
    WriteTempFile(L"aaa.md");
    CreateDir(L"zzz_dir");

    FileExplorer explorer;
    explorer.SetDirectory(temp_dir_.wstring());
    auto& entries = explorer.GetEntries();

    ASSERT_GE(entries.size(), 3u);
    // エントリ0: "..", エントリ1: ディレクトリ, エントリ2: ファイル
    EXPECT_TRUE(entries[0].is_parent());
    EXPECT_TRUE(entries[1].is_directory());
    EXPECT_FALSE(entries[2].is_directory());
}

TEST_F(FileExplorerTest, EntriesSortedCaseInsensitive)
{
    WriteTempFile(L"Bbb.md");
    WriteTempFile(L"aaa.md");
    WriteTempFile(L"ccc.md");

    FileExplorer explorer;
    explorer.SetDirectory(temp_dir_.wstring());
    auto& entries = explorer.GetEntries();

    // ".."エントリをスキップ
    ASSERT_GE(entries.size(), 4u);
    EXPECT_EQ(std::wstring_view(entries[1].GetDisplayName()), L"aaa.md");
    EXPECT_EQ(std::wstring_view(entries[2].GetDisplayName()), L"Bbb.md");
    EXPECT_EQ(std::wstring_view(entries[3].GetDisplayName()), L"ccc.md");
}

TEST_F(FileExplorerTest, CaseInsensitiveMdExtension)
{
    WriteTempFile(L"upper.MD");

    FileExplorer explorer;
    explorer.SetDirectory(temp_dir_.wstring());
    auto& entries = explorer.GetEntries();

    EXPECT_EQ(entries.size(), 2u); // ".." + ファイル1つ
}

// ---- SetCurrentFile テスト ----

TEST_F(FileExplorerTest, SetCurrentFileMarksEntry)
{
    WriteTempFile(L"a.md");
    WriteTempFile(L"b.md");

    FileExplorer explorer;
    explorer.SetDirectory(temp_dir_.wstring());
    std::wstring target = (temp_dir_ / L"b.md").wstring();
    explorer.SetCurrentFile(target);

    auto& entries = explorer.GetEntries();
    bool found = false;
    for (const auto& e : entries) {
        if (std::wstring_view(e.GetDisplayName()) == L"b.md") {
            EXPECT_TRUE(e.is_current());
            found = true;
        }
        else {
            EXPECT_FALSE(e.is_current());
        }
    }
    EXPECT_TRUE(found);
}

TEST_F(FileExplorerTest, SetCurrentFileDoesNotMarkDirectories)
{
    CreateDir(L"subdir");

    FileExplorer explorer;
    explorer.SetDirectory(temp_dir_.wstring());
    // パスが一致してもディレクトリはcurrentとしてマークされないこと
    std::wstring dir_path = (temp_dir_ / L"subdir").wstring();
    explorer.SetCurrentFile(dir_path);

    for (const auto& e : explorer.GetEntries()) {
        EXPECT_FALSE(e.is_current());
    }
}

// ---- ヒットテスト ----

TEST_F(FileExplorerTest, HitTestValidIndex)
{
    WriteTempFile(L"a.md");
    WriteTempFile(L"b.md");

    FileExplorer explorer;
    explorer.SetDirectory(temp_dir_.wstring());

    // 3エントリ: "..", "a.md", "b.md"
    EXPECT_EQ(explorer.HitTest(0.0f, 28.0f), 0);
    EXPECT_EQ(explorer.HitTest(28.0f, 28.0f), 1);
    EXPECT_EQ(explorer.HitTest(56.0f, 28.0f), 2);
}

TEST_F(FileExplorerTest, HitTestOutOfRange)
{
    WriteTempFile(L"a.md");

    FileExplorer explorer;
    explorer.SetDirectory(temp_dir_.wstring());

    EXPECT_EQ(explorer.HitTest(-1.0f, 28.0f), -1);
    EXPECT_EQ(explorer.HitTest(1000.0f, 28.0f), -1);
}

TEST_F(FileExplorerTest, HitTestZeroItemHeight)
{
    FileExplorer explorer;
    explorer.SetDirectory(temp_dir_.wstring());
    EXPECT_EQ(explorer.HitTest(10.0f, 0.0f), -1);
}

// ---- 初回起動シナリオ（ファイル未選択でディレクトリ表示） ----

TEST_F(FileExplorerTest, SetDirectoryWithoutCurrentFileHasNoCurrent)
{
    WriteTempFile(L"a.md");
    WriteTempFile(L"b.md");

    FileExplorer explorer;
    explorer.SetDirectory(temp_dir_.wstring());

    // SetCurrentFileを呼ばない場合、どのエントリもcurrentでないこと
    for (const auto& e : explorer.GetEntries()) {
        EXPECT_FALSE(e.is_current());
    }
}

TEST_F(FileExplorerTest, SetDirectoryWithCurrentWorkingDirectory)
{
    // 実際のカレントディレクトリを使用するシナリオ（初回起動を模倣）
    wchar_t cwd[MAX_PATH];
    ASSERT_NE(GetCurrentDirectoryW(MAX_PATH, cwd), 0u);

    FileExplorer explorer;
    explorer.SetDirectory(cwd);

    // ディレクトリが設定されていること
    EXPECT_FALSE(explorer.GetDirectory().empty());
    // 少なくとも ".." エントリがあること
    EXPECT_GE(explorer.GetEntries().size(), 1u);
    EXPECT_TRUE(explorer.GetEntries()[0].is_parent());
}

TEST_F(FileExplorerTest, SetDirectoryThenSetDirectoryAgainSwitches)
{
    // 初回起動でcwdを表示した後、ファイルのあるディレクトリに切り替えるシナリオ
    WriteTempFile(L"test.md");

    CreateDir(L"other");
    WriteTempFile(L"other/other.md");
    const fs::path other_dir = temp_dir_ / L"other";

    FileExplorer explorer;
    explorer.SetDirectory(temp_dir_.wstring());
    EXPECT_EQ(std::wstring_view{ explorer.GetDirectory() }, std::wstring_view{ temp_dir_.wstring() });

    // 別ディレクトリに切り替え
    explorer.SetDirectory(other_dir.wstring());
    EXPECT_EQ(std::wstring_view{ explorer.GetDirectory() }, std::wstring_view{ other_dir.wstring() });

    // 新しいディレクトリの内容が表示されること
    bool found_other = false;
    for (const auto& e : explorer.GetEntries()) {
        if (std::wstring_view(e.GetDisplayName()) == L"other.md") {
            found_other = true;
        }
        // 前のディレクトリのファイルがないこと
        EXPECT_NE(std::wstring_view(e.GetDisplayName()), L"test.md");
    }
    EXPECT_TRUE(found_other);
}

// ---- リフレッシュ / SetDirectory ----

TEST_F(FileExplorerTest, SetDirectorySamePathNoRefresh)
{
    WriteTempFile(L"a.md");

    FileExplorer explorer;
    explorer.SetDirectory(temp_dir_.wstring());
    size_t count1 = explorer.GetEntries().size();

    // 同じディレクトリを設定しても何も起こらないこと
    explorer.SetDirectory(temp_dir_.wstring());
    EXPECT_EQ(explorer.GetEntries().size(), count1);
}

TEST_F(FileExplorerTest, RefreshPicksUpNewFiles)
{
    FileExplorer explorer;
    explorer.SetDirectory(temp_dir_.wstring());
    size_t before = explorer.GetEntries().size();

    WriteTempFile(L"new.md");
    explorer.Refresh();

    EXPECT_EQ(explorer.GetEntries().size(), before + 1);
}

TEST_F(FileExplorerTest, FullPathIsCorrect)
{
    WriteTempFile(L"test.md");

    FileExplorer explorer;
    explorer.SetDirectory(temp_dir_.wstring());

    bool found = false;
    for (const auto& e : explorer.GetEntries()) {
        if (std::wstring_view(e.GetDisplayName()) == L"test.md") {
            std::wstring expected = temp_dir_.wstring() + L"\\" + L"test.md";
            EXPECT_EQ(std::wstring_view{ e.full_path }, std::wstring_view{ expected });
            found = true;
        }
    }
    EXPECT_TRUE(found);
}

TEST_F(FileExplorerTest, GetDirectoryReturnsSetPath)
{
    FileExplorer explorer;
    explorer.SetDirectory(temp_dir_.wstring());
    EXPECT_EQ(std::wstring_view{ explorer.GetDirectory() }, std::wstring_view{ temp_dir_.wstring() });
}
