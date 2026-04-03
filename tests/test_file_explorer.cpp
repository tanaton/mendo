#include <gtest/gtest.h>
#include <memory_resource>
#include <string_view>
#include "file_explorer.h"
#include <filesystem>
#include <fstream>

namespace fs = std::filesystem;

class FileExplorerTest : public ::testing::Test {
protected:
    fs::path temp_dir_;

    void SetUp() override
    {
        wchar_t tmp[MAX_PATH];
        GetTempPathW(MAX_PATH, tmp);
        // テストごとに一意のディレクトリを使用（CTest並列実行での競合を防止）
        auto* info = ::testing::UnitTest::GetInstance()->current_test_info();
        std::string suffix = std::string(info->test_suite_name()) + "_" + info->name();
        temp_dir_ = fs::path(tmp) / L"mendo_explorer_test" / fs::path(suffix);
        std::error_code ec;
        fs::remove_all(temp_dir_, ec);  // 前回の残りを確実に削除
        fs::create_directories(temp_dir_);
    }

    void TearDown() override
    {
        std::error_code ec;
        fs::remove_all(temp_dir_, ec);
    }

    void CreateFile(const std::wstring& name, const std::string& content = "")
    {
        auto path = temp_dir_ / name;
        std::ofstream f(path, std::ios::binary);
        f.write(content.data(), content.size());
        f.close();
    }

    void CreateDir(const std::wstring& name)
    {
        fs::create_directories(temp_dir_ / name);
    }
};

// ---- 基本的な列挙 ----

TEST_F(FileExplorerTest, EmptyDirectoryHasParentOnly)
{
    FileExplorer explorer;
    explorer.SetDirectory(temp_dir_.wstring());
    // ".." 親エントリのみ
    auto& entries = explorer.GetEntries();
    ASSERT_EQ(entries.size(), 1u);
    EXPECT_TRUE(entries[0].is_parent);
    EXPECT_EQ(entries[0].filename, L"..");
}

// ---- Bug #23: パスの正規化（末尾のバックスラッシュ） ----

TEST_F(FileExplorerTest, TrailingBackslashNormalized)
{
    CreateFile(L"test.md");
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
    CreateFile(L"hello.md");
    FileExplorer explorer;

    std::wstring with_slash = temp_dir_.wstring() + L"\\";
    explorer.SetDirectory(with_slash);

    // .mdファイルが問題なく見つかること
    bool found_md = false;
    for (const auto& entry : explorer.GetEntries()) {
        if (entry.filename == L"hello.md") {
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
    CreateFile(L"readme.md");
    CreateFile(L"notes.md");

    FileExplorer explorer;
    explorer.SetDirectory(temp_dir_.wstring());
    auto& entries = explorer.GetEntries();

    // ".." + mdファイル2つ
    EXPECT_EQ(entries.size(), 3u);
}

TEST_F(FileExplorerTest, ShowsMarkdownExtension)
{
    CreateFile(L"doc.markdown");

    FileExplorer explorer;
    explorer.SetDirectory(temp_dir_.wstring());
    auto& entries = explorer.GetEntries();

    // ".." + markdownファイル1つ
    EXPECT_EQ(entries.size(), 2u);
    EXPECT_EQ(entries[1].filename, L"doc.markdown");
}

TEST_F(FileExplorerTest, ShowsMkdExtension)
{
    CreateFile(L"doc.mkd");

    FileExplorer explorer;
    explorer.SetDirectory(temp_dir_.wstring());
    auto& entries = explorer.GetEntries();

    EXPECT_EQ(entries.size(), 2u);
    EXPECT_EQ(entries[1].filename, L"doc.mkd");
}

TEST_F(FileExplorerTest, HidesNonMarkdownFiles)
{
    CreateFile(L"readme.md");
    CreateFile(L"image.png");
    CreateFile(L"data.json");
    CreateFile(L"script.py");

    FileExplorer explorer;
    explorer.SetDirectory(temp_dir_.wstring());
    auto& entries = explorer.GetEntries();

    // ".." + mdファイル1つ（非mdファイルは非表示）
    EXPECT_EQ(entries.size(), 2u);
}

TEST_F(FileExplorerTest, ShowsDirectories)
{
    CreateDir(L"subdir");
    CreateFile(L"test.md");

    FileExplorer explorer;
    explorer.SetDirectory(temp_dir_.wstring());
    auto& entries = explorer.GetEntries();

    // ".." + ディレクトリ1つ + ファイル1つ
    EXPECT_EQ(entries.size(), 3u);
}

TEST_F(FileExplorerTest, DirectoriesBeforeFiles)
{
    CreateFile(L"aaa.md");
    CreateDir(L"zzz_dir");

    FileExplorer explorer;
    explorer.SetDirectory(temp_dir_.wstring());
    auto& entries = explorer.GetEntries();

    ASSERT_GE(entries.size(), 3u);
    // エントリ0: "..", エントリ1: ディレクトリ, エントリ2: ファイル
    EXPECT_TRUE(entries[0].is_parent);
    EXPECT_TRUE(entries[1].is_directory);
    EXPECT_FALSE(entries[2].is_directory);
}

TEST_F(FileExplorerTest, EntriesSortedCaseInsensitive)
{
    CreateFile(L"Bbb.md");
    CreateFile(L"aaa.md");
    CreateFile(L"ccc.md");

    FileExplorer explorer;
    explorer.SetDirectory(temp_dir_.wstring());
    auto& entries = explorer.GetEntries();

    // ".."エントリをスキップ
    ASSERT_GE(entries.size(), 4u);
    EXPECT_EQ(entries[1].filename, L"aaa.md");
    EXPECT_EQ(entries[2].filename, L"Bbb.md");
    EXPECT_EQ(entries[3].filename, L"ccc.md");
}

TEST_F(FileExplorerTest, CaseInsensitiveMdExtension)
{
    CreateFile(L"upper.MD");

    FileExplorer explorer;
    explorer.SetDirectory(temp_dir_.wstring());
    auto& entries = explorer.GetEntries();

    EXPECT_EQ(entries.size(), 2u); // ".." + ファイル1つ
}

// ---- SetCurrentFile テスト ----

TEST_F(FileExplorerTest, SetCurrentFileMarksEntry)
{
    CreateFile(L"a.md");
    CreateFile(L"b.md");

    FileExplorer explorer;
    explorer.SetDirectory(temp_dir_.wstring());
    std::wstring target = (temp_dir_ / L"b.md").wstring();
    explorer.SetCurrentFile(target);

    auto& entries = explorer.GetEntries();
    bool found = false;
    for (const auto& e : entries) {
        if (e.filename == L"b.md") {
            EXPECT_TRUE(e.is_current);
            found = true;
        }
        else {
            EXPECT_FALSE(e.is_current);
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
        EXPECT_FALSE(e.is_current);
    }
}

// ---- ヒットテスト ----

TEST_F(FileExplorerTest, HitTestValidIndex)
{
    CreateFile(L"a.md");
    CreateFile(L"b.md");

    FileExplorer explorer;
    explorer.SetDirectory(temp_dir_.wstring());

    // 3エントリ: "..", "a.md", "b.md"
    EXPECT_EQ(explorer.HitTest(0.0f, 28.0f), 0);
    EXPECT_EQ(explorer.HitTest(28.0f, 28.0f), 1);
    EXPECT_EQ(explorer.HitTest(56.0f, 28.0f), 2);
}

TEST_F(FileExplorerTest, HitTestOutOfRange)
{
    CreateFile(L"a.md");

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
    CreateFile(L"a.md");
    CreateFile(L"b.md");

    FileExplorer explorer;
    explorer.SetDirectory(temp_dir_.wstring());

    // SetCurrentFileを呼ばない場合、どのエントリもcurrentでないこと
    for (const auto& e : explorer.GetEntries()) {
        EXPECT_FALSE(e.is_current);
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
    EXPECT_TRUE(explorer.GetEntries()[0].is_parent);
}

TEST_F(FileExplorerTest, SetDirectoryThenSetDirectoryAgainSwitches)
{
    // 初回起動でcwdを表示した後、ファイルのあるディレクトリに切り替えるシナリオ
    CreateFile(L"test.md");

    fs::path other_dir = temp_dir_ / L"other";
    fs::create_directories(other_dir);
    std::ofstream(other_dir / L"other.md", std::ios::binary).close();

    FileExplorer explorer;
    explorer.SetDirectory(temp_dir_.wstring());
    EXPECT_EQ(std::wstring_view{ explorer.GetDirectory() }, std::wstring_view{ temp_dir_.wstring() });

    // 別ディレクトリに切り替え
    explorer.SetDirectory(other_dir.wstring());
    EXPECT_EQ(std::wstring_view{ explorer.GetDirectory() }, std::wstring_view{ other_dir.wstring() });

    // 新しいディレクトリの内容が表示されること
    bool found_other = false;
    for (const auto& e : explorer.GetEntries()) {
        if (e.filename == L"other.md") {
            found_other = true;
        }
        // 前のディレクトリのファイルがないこと
        EXPECT_NE(e.filename, L"test.md");
    }
    EXPECT_TRUE(found_other);
}

// ---- リフレッシュ / SetDirectory ----

TEST_F(FileExplorerTest, SetDirectorySamePathNoRefresh)
{
    CreateFile(L"a.md");

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

    CreateFile(L"new.md");
    explorer.Refresh();

    EXPECT_EQ(explorer.GetEntries().size(), before + 1);
}

TEST_F(FileExplorerTest, FullPathIsCorrect)
{
    CreateFile(L"test.md");

    FileExplorer explorer;
    explorer.SetDirectory(temp_dir_.wstring());

    bool found = false;
    for (const auto& e : explorer.GetEntries()) {
        if (e.filename == L"test.md") {
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
