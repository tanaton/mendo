#include <gtest/gtest.h>
#include "file_explorer.h"
#include <filesystem>
#include <fstream>

namespace fs = std::filesystem;

class FileExplorerTest : public ::testing::Test {
protected:
    fs::path temp_dir_;

    void SetUp() override {
        wchar_t tmp[MAX_PATH];
        GetTempPathW(MAX_PATH, tmp);
        temp_dir_ = fs::path(tmp) / L"mendo_explorer_test";
        fs::create_directories(temp_dir_);
    }

    void TearDown() override {
        fs::remove_all(temp_dir_);
    }

    void CreateFile(const std::wstring& name, const std::string& content = "") {
        auto path = temp_dir_ / name;
        std::ofstream f(path, std::ios::binary);
        f.write(content.data(), content.size());
        f.close();
    }

    void CreateDir(const std::wstring& name) {
        fs::create_directories(temp_dir_ / name);
    }
};

// ---- Basic enumeration ----

TEST_F(FileExplorerTest, EmptyDirectoryHasParentOnly) {
    FileExplorer explorer;
    explorer.SetDirectory(temp_dir_.wstring());
    // Only the ".." parent entry
    auto& entries = explorer.GetEntries();
    ASSERT_EQ(entries.size(), 1u);
    EXPECT_TRUE(entries[0].is_parent);
    EXPECT_EQ(entries[0].filename, L"..");
}

// ---- Bug #23: Path normalization (trailing backslash) ----

TEST_F(FileExplorerTest, TrailingBackslashNormalized) {
    CreateFile(L"test.md");
    FileExplorer explorer;

    std::wstring with_slash = temp_dir_.wstring() + L"\\";
    std::wstring without_slash = temp_dir_.wstring();

    explorer.SetDirectory(with_slash);
    size_t count1 = explorer.GetEntries().size();

    // Setting same directory with different trailing slash should not re-refresh
    // (if normalization works, the directory_ comparison detects same dir)
    explorer.SetDirectory(without_slash);
    size_t count2 = explorer.GetEntries().size();

    EXPECT_EQ(count1, count2);
}

TEST_F(FileExplorerTest, TrailingSlashDoesNotCreateDoubleBackslash) {
    CreateFile(L"hello.md");
    FileExplorer explorer;

    std::wstring with_slash = temp_dir_.wstring() + L"\\";
    explorer.SetDirectory(with_slash);

    // Should find the .md file without issues
    bool found_md = false;
    for (const auto& entry : explorer.GetEntries()) {
        if (entry.filename == L"hello.md") {
            found_md = true;
            // full_path should not have double backslash
            EXPECT_EQ(entry.full_path.find(L"\\\\"), std::wstring::npos)
                << "full_path should not contain double backslash: "
                << std::string(entry.full_path.begin(), entry.full_path.end());
        }
    }
    EXPECT_TRUE(found_md);
}

TEST_F(FileExplorerTest, ShowsMdFiles) {
    CreateFile(L"readme.md");
    CreateFile(L"notes.md");

    FileExplorer explorer;
    explorer.SetDirectory(temp_dir_.wstring());
    auto& entries = explorer.GetEntries();

    // ".." + 2 md files
    EXPECT_EQ(entries.size(), 3u);
}

TEST_F(FileExplorerTest, ShowsMarkdownExtension) {
    CreateFile(L"doc.markdown");

    FileExplorer explorer;
    explorer.SetDirectory(temp_dir_.wstring());
    auto& entries = explorer.GetEntries();

    // ".." + 1 markdown file
    EXPECT_EQ(entries.size(), 2u);
    EXPECT_EQ(entries[1].filename, L"doc.markdown");
}

TEST_F(FileExplorerTest, ShowsMkdExtension) {
    CreateFile(L"doc.mkd");

    FileExplorer explorer;
    explorer.SetDirectory(temp_dir_.wstring());
    auto& entries = explorer.GetEntries();

    EXPECT_EQ(entries.size(), 2u);
    EXPECT_EQ(entries[1].filename, L"doc.mkd");
}

TEST_F(FileExplorerTest, HidesNonMarkdownFiles) {
    CreateFile(L"readme.md");
    CreateFile(L"image.png");
    CreateFile(L"data.json");
    CreateFile(L"script.py");

    FileExplorer explorer;
    explorer.SetDirectory(temp_dir_.wstring());
    auto& entries = explorer.GetEntries();

    // ".." + 1 md file (non-md files hidden)
    EXPECT_EQ(entries.size(), 2u);
}

TEST_F(FileExplorerTest, ShowsDirectories) {
    CreateDir(L"subdir");
    CreateFile(L"test.md");

    FileExplorer explorer;
    explorer.SetDirectory(temp_dir_.wstring());
    auto& entries = explorer.GetEntries();

    // ".." + 1 dir + 1 file
    EXPECT_EQ(entries.size(), 3u);
}

TEST_F(FileExplorerTest, DirectoriesBeforeFiles) {
    CreateFile(L"aaa.md");
    CreateDir(L"zzz_dir");

    FileExplorer explorer;
    explorer.SetDirectory(temp_dir_.wstring());
    auto& entries = explorer.GetEntries();

    ASSERT_GE(entries.size(), 3u);
    // Entry 0: "..", Entry 1: directory, Entry 2: file
    EXPECT_TRUE(entries[0].is_parent);
    EXPECT_TRUE(entries[1].is_directory);
    EXPECT_FALSE(entries[2].is_directory);
}

TEST_F(FileExplorerTest, EntriesSortedCaseInsensitive) {
    CreateFile(L"Bbb.md");
    CreateFile(L"aaa.md");
    CreateFile(L"ccc.md");

    FileExplorer explorer;
    explorer.SetDirectory(temp_dir_.wstring());
    auto& entries = explorer.GetEntries();

    // Skip ".." entry
    ASSERT_GE(entries.size(), 4u);
    EXPECT_EQ(entries[1].filename, L"aaa.md");
    EXPECT_EQ(entries[2].filename, L"Bbb.md");
    EXPECT_EQ(entries[3].filename, L"ccc.md");
}

TEST_F(FileExplorerTest, CaseInsensitiveMdExtension) {
    CreateFile(L"upper.MD");

    FileExplorer explorer;
    explorer.SetDirectory(temp_dir_.wstring());
    auto& entries = explorer.GetEntries();

    EXPECT_EQ(entries.size(), 2u); // ".." + 1 file
}

// ---- SetCurrentFile ----

TEST_F(FileExplorerTest, SetCurrentFileMarksEntry) {
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
        } else {
            EXPECT_FALSE(e.is_current);
        }
    }
    EXPECT_TRUE(found);
}

TEST_F(FileExplorerTest, SetCurrentFileDoesNotMarkDirectories) {
    CreateDir(L"subdir");

    FileExplorer explorer;
    explorer.SetDirectory(temp_dir_.wstring());
    // Even if path matches, directories should not be marked as current
    std::wstring dir_path = (temp_dir_ / L"subdir").wstring();
    explorer.SetCurrentFile(dir_path);

    for (const auto& e : explorer.GetEntries()) {
        EXPECT_FALSE(e.is_current);
    }
}

// ---- HitTest ----

TEST_F(FileExplorerTest, HitTestValidIndex) {
    CreateFile(L"a.md");
    CreateFile(L"b.md");

    FileExplorer explorer;
    explorer.SetDirectory(temp_dir_.wstring());

    // 3 entries: "..", "a.md", "b.md"
    EXPECT_EQ(explorer.HitTest(0.0f, 28.0f), 0);
    EXPECT_EQ(explorer.HitTest(28.0f, 28.0f), 1);
    EXPECT_EQ(explorer.HitTest(56.0f, 28.0f), 2);
}

TEST_F(FileExplorerTest, HitTestOutOfRange) {
    CreateFile(L"a.md");

    FileExplorer explorer;
    explorer.SetDirectory(temp_dir_.wstring());

    EXPECT_EQ(explorer.HitTest(-1.0f, 28.0f), -1);
    EXPECT_EQ(explorer.HitTest(1000.0f, 28.0f), -1);
}

TEST_F(FileExplorerTest, HitTestZeroItemHeight) {
    FileExplorer explorer;
    explorer.SetDirectory(temp_dir_.wstring());
    EXPECT_EQ(explorer.HitTest(10.0f, 0.0f), -1);
}

// ---- Refresh / SetDirectory ----

TEST_F(FileExplorerTest, SetDirectorySamePathNoRefresh) {
    CreateFile(L"a.md");

    FileExplorer explorer;
    explorer.SetDirectory(temp_dir_.wstring());
    size_t count1 = explorer.GetEntries().size();

    // Setting same directory should be a no-op
    explorer.SetDirectory(temp_dir_.wstring());
    EXPECT_EQ(explorer.GetEntries().size(), count1);
}

TEST_F(FileExplorerTest, RefreshPicksUpNewFiles) {
    FileExplorer explorer;
    explorer.SetDirectory(temp_dir_.wstring());
    size_t before = explorer.GetEntries().size();

    CreateFile(L"new.md");
    explorer.Refresh();

    EXPECT_EQ(explorer.GetEntries().size(), before + 1);
}

TEST_F(FileExplorerTest, FullPathIsCorrect) {
    CreateFile(L"test.md");

    FileExplorer explorer;
    explorer.SetDirectory(temp_dir_.wstring());

    bool found = false;
    for (const auto& e : explorer.GetEntries()) {
        if (e.filename == L"test.md") {
            std::wstring expected = temp_dir_.wstring() + L"\\" + L"test.md";
            EXPECT_EQ(e.full_path, expected);
            found = true;
        }
    }
    EXPECT_TRUE(found);
}

TEST_F(FileExplorerTest, GetDirectoryReturnsSetPath) {
    FileExplorer explorer;
    explorer.SetDirectory(temp_dir_.wstring());
    EXPECT_EQ(explorer.GetDirectory(), temp_dir_.wstring());
}
