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

TEST(StartupPlanTest, NoArg_SystemCwd_FallsBackToLastFileDirectory)
{
    auto ctx = MakeContext(StartupArgKind::None);
    ctx.cwd = L"C:\\Windows\\System32";
    EXPECT_EQ(DecideStartupPlan(ctx).pane_directory, L"D:\\notes");
}

TEST(StartupPlanTest, NoArg_ExeDirCwd_FallsBackToLastFileDirectory)
{
    auto ctx = MakeContext(StartupArgKind::None);
    ctx.cwd = L"C:\\Tools\\mendo";
    EXPECT_EQ(DecideStartupPlan(ctx).pane_directory, L"D:\\notes");
}

TEST(StartupPlanTest, NoArg_IgnoredCwdComparisonIsCaseAndTrailingSeparatorInsensitive)
{
    auto ctx = MakeContext(StartupArgKind::None);
    ctx.cwd = L"c:\\windows\\SYSTEM32\\";
    EXPECT_EQ(DecideStartupPlan(ctx).pane_directory, L"D:\\notes");
}

TEST(StartupPlanTest, NoArg_SubdirectoryOfIgnoredCwdIsUsed)
{
    auto ctx = MakeContext(StartupArgKind::None);
    ctx.cwd = L"C:\\Windows\\System32\\drivers";
    EXPECT_EQ(DecideStartupPlan(ctx).pane_directory, L"C:\\Windows\\System32\\drivers");
}

TEST(StartupPlanTest, NoArg_IgnoredCwdWithoutLastFile_KeepsCwd)
{
    auto ctx = MakeContext(StartupArgKind::None);
    ctx.cwd = L"C:\\Windows\\System32";
    ctx.last_file = {};
    EXPECT_EQ(DecideStartupPlan(ctx).pane_directory, L"C:\\Windows\\System32");
}

TEST(StartupPlanTest, NoArg_EmptyCwd_FallsBackToLastFileDirectory)
{
    auto ctx = MakeContext(StartupArgKind::None);
    ctx.cwd = {};
    EXPECT_EQ(DecideStartupPlan(ctx).pane_directory, L"D:\\notes");
}

// ---- ファイル指定 ----

TEST(StartupPlanTest, FileArg_OpensFileAndShowsItsDirectory)
{
    const auto plan = DecideStartupPlan(MakeContext(StartupArgKind::File, L"E:\\proj\\README.md"));
    EXPECT_EQ(plan.document_path, L"E:\\proj\\README.md");
    EXPECT_FALSE(plan.restore_scroll);
    EXPECT_EQ(plan.pane_directory, L"E:\\proj");
}

TEST(StartupPlanTest, FileArg_SameAsLastFile_RestoresScroll)
{
    auto ctx = MakeContext(StartupArgKind::File, kLastFile);
    ctx.arg_is_last_file = true;
    const auto plan = DecideStartupPlan(ctx);
    EXPECT_EQ(plan.document_path, kLastFile);
    EXPECT_TRUE(plan.restore_scroll);
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
