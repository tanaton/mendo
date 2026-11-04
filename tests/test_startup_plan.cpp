#include <gtest/gtest.h>
#include "startup_plan.h"
#include <array>

namespace {

constexpr std::wstring_view kCwd = L"C:\\work\\docs";
constexpr std::wstring_view kLastFile = L"D:\\notes\\memo.md";
constexpr std::wstring_view kReadme = L"E:\\proj\\README.md";
constexpr std::wstring_view kSystem = L"C:\\Windows\\System32";
constexpr std::array<std::wstring_view, 2> kIgnored = { kSystem, L"C:\\Tools\\mendo" };

} // namespace

// 引数なしとフォルダ指定は前回のファイルを復元し (無ければヘルプ)、ファイル指定はそのファイルを開く。無効な引数はヘルプ。
// 前回のファイルと同じならスクロール位置も戻す。表記違いの同一ファイルかもしれないときだけ、呼び出し側が開いて確かめる。
// ファイルペインは指定のフォルダ、ファイルのフォルダ、cwd の順に出す。システムフォルダ・exe のフォルダ・空の cwd は
// 作業場所とみなさず前回のファイルのフォルダを出すが、それらの下のフォルダは作業場所として使う。
TEST(StartupPlanTest, DecidesDocumentScrollAndPane)
{
    struct Case {
        const char* name;
        StartupArgKind kind;
        std::wstring_view arg = {};
        std::wstring_view cwd = kCwd;
        std::wstring_view last_file = kLastFile;
        std::wstring_view document = {};
        bool restore = false;
        bool restore_if_same = false;
        std::wstring_view pane = {};
    };
    using enum StartupArgKind;
    const Case kCases[] = {
        { .name = "NoArg", .kind = None, .document = kLastFile, .restore = true, .pane = kCwd },
        { .name = "NoArgNoLastFile", .kind = None, .last_file = L"", .pane = kCwd },
        { .name = "SystemCwd", .kind = None, .cwd = kSystem, .document = kLastFile, .restore = true, .pane = L"D:\\notes" },
        { .name = "ExeDirCwd", .kind = None, .cwd = L"C:\\Tools\\mendo", .document = kLastFile, .restore = true, .pane = L"D:\\notes" },
        { .name = "IgnoredCwdCaseAndTrailingSeparator", .kind = None, .cwd = L"c:\\windows\\SYSTEM32\\", .document = kLastFile, .restore = true, .pane = L"D:\\notes" },
        { .name = "EmptyCwd", .kind = None, .cwd = L"", .document = kLastFile, .restore = true, .pane = L"D:\\notes" },
        { .name = "SubdirectoryOfIgnoredCwd", .kind = None, .cwd = L"C:\\Windows\\System32\\drivers", .document = kLastFile, .restore = true, .pane = L"C:\\Windows\\System32\\drivers" },
        { .name = "IgnoredCwdWithoutLastFile", .kind = None, .cwd = kSystem, .last_file = L"", .pane = kSystem },
        // ファイル名が前回と違えば、開いて確かめるまでもなく別のファイル。
        { .name = "File", .kind = File, .arg = kReadme, .document = kReadme, .pane = L"E:\\proj" },
        { .name = "FileSameAsLastFile", .kind = File, .arg = kLastFile, .document = kLastFile, .restore = true, .pane = L"D:\\notes" },
        { .name = "FileSlashAndCase", .kind = File, .arg = L"d:/notes/MEMO.md", .document = L"d:/notes/MEMO.md", .restore_if_same = true, .pane = L"d:/notes" },
        { .name = "FileShortDirName", .kind = File, .arg = L"D:\\NOTES~1\\memo.md", .document = L"D:\\NOTES~1\\memo.md", .restore_if_same = true, .pane = L"D:\\NOTES~1" },
        { .name = "FileShortFileName", .kind = File, .arg = L"D:\\notes\\MEMO~1.MD", .document = L"D:\\notes\\MEMO~1.MD", .restore_if_same = true, .pane = L"D:\\notes" },
        // 比べる相手が無いので、ファイルを開いて確かめる必要も無い。
        { .name = "FileNoLastFile", .kind = File, .arg = kReadme, .last_file = L"", .document = kReadme, .pane = L"E:\\proj" },
        { .name = "FileNonMarkdown", .kind = File, .arg = L"E:\\proj\\notes.txt", .document = L"E:\\proj\\notes.txt", .pane = L"E:\\proj" },
        { .name = "FileInRoot", .kind = File, .arg = L"E:\\README.md", .document = L"E:\\README.md", .pane = L"E:\\" },
        { .name = "Directory", .kind = Directory, .arg = L"E:\\proj", .document = kLastFile, .restore = true, .pane = L"E:\\proj" },
        { .name = "DirectoryNoLastFile", .kind = Directory, .arg = L"E:\\proj", .last_file = L"", .pane = L"E:\\proj" },
        { .name = "Invalid", .kind = Invalid, .arg = L"C:\\work\\docs\\missing.md", .pane = kCwd },
    };
    for (const auto& c : kCases) {
        SCOPED_TRACE(c.name);
        const auto plan = DecideStartupPlan({
            .arg_kind = c.kind,
            .arg_path = c.arg,
            .last_file = c.last_file,
            .cwd = c.cwd,
            .ignored_cwds = kIgnored,
        });
        EXPECT_EQ(plan.document_path, c.document);
        EXPECT_EQ(plan.restore_scroll, c.restore);
        EXPECT_EQ(plan.restore_scroll_if_same_file, c.restore_if_same);
        EXPECT_EQ(plan.pane_directory, c.pane);
    }
}
