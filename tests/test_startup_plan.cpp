#include <gtest/gtest.h>
#include "startup_plan.h"
#include <array>

namespace {

constexpr std::wstring_view kCwd = L"C:\\work\\docs";
constexpr std::wstring_view kLastFile = L"D:\\notes\\memo.md";
constexpr std::array<std::wstring_view, 2> kIgnored = { L"C:\\Windows\\System32", L"C:\\Tools\\mendo" };

StartupContext MakeContext(StartupArgKind kind, std::wstring_view arg = {})
{
    return {
        .arg_kind = kind,
        .arg_path = arg,
        .last_file = kLastFile,
        .cwd = kCwd,
        .ignored_cwds = kIgnored,
    };
}

} // namespace

// ---- 引数なし ----

TEST(StartupPlanTest, NoArg_RestoresLastFileAndShowsCwd)
{
    const auto plan = DecideStartupPlan(MakeContext(StartupArgKind::None));
    EXPECT_EQ(plan.document_path, kLastFile);
    EXPECT_TRUE(plan.restore_scroll);
    EXPECT_EQ(plan.pane_directory, kCwd);
}

TEST(StartupPlanTest, NoArg_NoLastFile_ShowsHelpAndCwd)
{
    auto ctx = MakeContext(StartupArgKind::None);
    ctx.last_file = {};
    const auto plan = DecideStartupPlan(ctx);
    EXPECT_TRUE(plan.document_path.empty());
    EXPECT_FALSE(plan.restore_scroll);
    EXPECT_EQ(plan.pane_directory, kCwd);
}

// システムフォルダ・exe のフォルダ・空の cwd は作業場所とみなさず、前回ファイルのフォルダを出す。
// それらの下のフォルダは作業場所として使う。
TEST(StartupPlanTest, NoArg_PaneDirectoryForIgnoredCwd)
{
    struct Case {
        const char* name;
        std::wstring_view cwd;
        std::wstring_view expected;
    };
    const Case kCases[] = {
        { "SystemCwd", L"C:\\Windows\\System32", L"D:\\notes" },
        { "ExeDirCwd", L"C:\\Tools\\mendo", L"D:\\notes" },
        { "CaseAndTrailingSeparatorInsensitive", L"c:\\windows\\SYSTEM32\\", L"D:\\notes" },
        { "EmptyCwd", L"", L"D:\\notes" },
        { "SubdirectoryOfIgnoredCwdIsUsed", L"C:\\Windows\\System32\\drivers", L"C:\\Windows\\System32\\drivers" },
    };
    for (const auto& c : kCases) {
        SCOPED_TRACE(c.name);
        auto ctx = MakeContext(StartupArgKind::None);
        ctx.cwd = c.cwd;
        EXPECT_EQ(DecideStartupPlan(ctx).pane_directory, c.expected);
    }
}

TEST(StartupPlanTest, NoArg_IgnoredCwdWithoutLastFile_KeepsCwd)
{
    auto ctx = MakeContext(StartupArgKind::None);
    ctx.cwd = L"C:\\Windows\\System32";
    ctx.last_file = {};
    EXPECT_EQ(DecideStartupPlan(ctx).pane_directory, L"C:\\Windows\\System32");
}

// ---- ファイル指定 ----

TEST(StartupPlanTest, FileArg_OpensFileAndShowsItsDirectory)
{
    const auto plan = DecideStartupPlan(MakeContext(StartupArgKind::File, L"E:\\proj\\README.md"));
    EXPECT_EQ(plan.document_path, L"E:\\proj\\README.md");
    EXPECT_FALSE(plan.restore_scroll);
    // 表記違いの同一ファイルかは呼び出し側が確かめる。
    EXPECT_TRUE(plan.restore_scroll_if_same_file);
    EXPECT_EQ(plan.pane_directory, L"E:\\proj");
}

TEST(StartupPlanTest, FileArg_SameAsLastFile_RestoresScroll)
{
    auto ctx = MakeContext(StartupArgKind::File, kLastFile);
    ctx.arg_is_last_file = true;
    const auto plan = DecideStartupPlan(ctx);
    EXPECT_EQ(plan.document_path, kLastFile);
    EXPECT_TRUE(plan.restore_scroll);
    EXPECT_FALSE(plan.restore_scroll_if_same_file);
}

// 比べる相手が無いので、ファイルを開いて確かめる必要も無い。
TEST(StartupPlanTest, FileArg_NoLastFile_SkipsSameFileCheck)
{
    auto ctx = MakeContext(StartupArgKind::File, L"E:\\proj\\README.md");
    ctx.last_file = {};
    const auto plan = DecideStartupPlan(ctx);
    EXPECT_FALSE(plan.restore_scroll);
    EXPECT_FALSE(plan.restore_scroll_if_same_file);
}

TEST(StartupPlanTest, FileArg_NonMarkdownFileIsOpenedAsBefore)
{
    const auto plan = DecideStartupPlan(MakeContext(StartupArgKind::File, L"E:\\proj\\notes.txt"));
    EXPECT_EQ(plan.document_path, L"E:\\proj\\notes.txt");
    EXPECT_EQ(plan.pane_directory, L"E:\\proj");
}

TEST(StartupPlanTest, FileArg_InRootDirectory_ShowsRoot)
{
    const auto plan = DecideStartupPlan(MakeContext(StartupArgKind::File, L"E:\\README.md"));
    EXPECT_EQ(plan.pane_directory, L"E:\\");
}

// ---- フォルダ指定 ----

TEST(StartupPlanTest, DirectoryArg_ShowsDirectoryAndRestoresLastFile)
{
    const auto plan = DecideStartupPlan(MakeContext(StartupArgKind::Directory, L"E:\\proj"));
    EXPECT_EQ(plan.document_path, kLastFile);
    EXPECT_TRUE(plan.restore_scroll);
    EXPECT_EQ(plan.pane_directory, L"E:\\proj");
}

TEST(StartupPlanTest, DirectoryArg_NoLastFile_ShowsHelp)
{
    auto ctx = MakeContext(StartupArgKind::Directory, L"E:\\proj");
    ctx.last_file = {};
    const auto plan = DecideStartupPlan(ctx);
    EXPECT_TRUE(plan.document_path.empty());
    EXPECT_FALSE(plan.restore_scroll);
    EXPECT_EQ(plan.pane_directory, L"E:\\proj");
}

// ---- 無効な引数 ----

TEST(StartupPlanTest, InvalidArg_ShowsHelpWithoutRestoring)
{
    const auto plan = DecideStartupPlan(MakeContext(StartupArgKind::Invalid, L"C:\\work\\docs\\missing.md"));
    EXPECT_TRUE(plan.document_path.empty());
    EXPECT_FALSE(plan.restore_scroll);
    EXPECT_EQ(plan.pane_directory, kCwd);
}
