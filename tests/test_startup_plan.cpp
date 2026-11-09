#include <gtest/gtest.h>
#include "startup_plan.h"
#include <array>
#include <vector>

namespace {

constexpr std::wstring_view kCwd = L"C:\\work\\docs";
constexpr std::wstring_view kLastFile = L"D:\\notes\\memo.md";
constexpr std::wstring_view kReadme = L"E:\\proj\\README.md";
constexpr std::wstring_view kSystem = L"C:\\Windows\\System32";
constexpr std::array<std::wstring_view, 2> kIgnored = { kSystem, L"C:\\Tools\\mendo" };
const StartupScroll kRestore = startup_scroll::Restore{};
const StartupScroll kRestoreIfSame = startup_scroll::RestoreIfSameFile{};

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
        std::wstring_view anchor = {};
        std::wstring_view cwd = kCwd;
        std::wstring_view last_file = kLastFile;
        std::wstring_view document = {};
        StartupScroll scroll = {};
        std::wstring_view pane = {};
    };
    using enum StartupArgKind;
    const Case kCases[] = {
        { .name = "NoArg", .kind = None, .document = kLastFile, .scroll = kRestore, .pane = kCwd },
        { .name = "NoArgNoLastFile", .kind = None, .last_file = L"", .pane = kCwd },
        { .name = "SystemCwd", .kind = None, .cwd = kSystem, .document = kLastFile, .scroll = kRestore, .pane = L"D:\\notes" },
        { .name = "ExeDirCwd", .kind = None, .cwd = L"C:\\Tools\\mendo", .document = kLastFile, .scroll = kRestore, .pane = L"D:\\notes" },
        { .name = "IgnoredCwdCaseAndTrailingSeparator", .kind = None, .cwd = L"c:\\windows\\SYSTEM32\\", .document = kLastFile, .scroll = kRestore, .pane = L"D:\\notes" },
        { .name = "EmptyCwd", .kind = None, .cwd = L"", .document = kLastFile, .scroll = kRestore, .pane = L"D:\\notes" },
        { .name = "SubdirectoryOfIgnoredCwd", .kind = None, .cwd = L"C:\\Windows\\System32\\drivers", .document = kLastFile, .scroll = kRestore, .pane = L"C:\\Windows\\System32\\drivers" },
        { .name = "IgnoredCwdWithoutLastFile", .kind = None, .cwd = kSystem, .last_file = L"", .pane = kSystem },
        // ファイル名が前回と違えば、開いて確かめるまでもなく別のファイル。
        { .name = "File", .kind = File, .arg = kReadme, .document = kReadme, .pane = L"E:\\proj" },
        { .name = "FileSameAsLastFile", .kind = File, .arg = kLastFile, .document = kLastFile, .scroll = kRestore, .pane = L"D:\\notes" },
        { .name = "FileSlashAndCase", .kind = File, .arg = L"d:/notes/MEMO.md", .document = L"d:/notes/MEMO.md", .scroll = kRestoreIfSame, .pane = L"d:/notes" },
        { .name = "FileShortDirName", .kind = File, .arg = L"D:\\NOTES~1\\memo.md", .document = L"D:\\NOTES~1\\memo.md", .scroll = kRestoreIfSame, .pane = L"D:\\NOTES~1" },
        { .name = "FileShortFileName", .kind = File, .arg = L"D:\\notes\\MEMO~1.MD", .document = L"D:\\notes\\MEMO~1.MD", .scroll = kRestoreIfSame, .pane = L"D:\\notes" },
        // 比べる相手が無いので、ファイルを開いて確かめる必要も無い。
        { .name = "FileNoLastFile", .kind = File, .arg = kReadme, .last_file = L"", .document = kReadme, .pane = L"E:\\proj" },
        { .name = "FileNonMarkdown", .kind = File, .arg = L"E:\\proj\\notes.txt", .document = L"E:\\proj\\notes.txt", .pane = L"E:\\proj" },
        { .name = "FileInRoot", .kind = File, .arg = L"E:\\README.md", .document = L"E:\\README.md", .pane = L"E:\\" },
        { .name = "Directory", .kind = Directory, .arg = L"E:\\proj", .document = kLastFile, .scroll = kRestore, .pane = L"E:\\proj" },
        { .name = "DirectoryNoLastFile", .kind = Directory, .arg = L"E:\\proj", .last_file = L"", .pane = L"E:\\proj" },
        { .name = "Invalid", .kind = Invalid, .arg = L"C:\\work\\docs\\missing.md", .pane = kCwd },
        // 見出しはファイルを指定したときだけ使い、前回のスクロール位置より優先する。
        { .name = "FileWithAnchor", .kind = File, .arg = kReadme, .anchor = L"usage", .document = kReadme, .scroll = startup_scroll::Anchor{ L"usage" }, .pane = L"E:\\proj" },
        { .name = "AnchorOverridesRestore", .kind = File, .arg = kLastFile, .anchor = L"usage", .document = kLastFile, .scroll = startup_scroll::Anchor{ L"usage" }, .pane = L"D:\\notes" },
        { .name = "AnchorWithDirectory", .kind = Directory, .arg = L"E:\\proj", .anchor = L"usage", .document = kLastFile, .scroll = kRestore, .pane = L"E:\\proj" },
        { .name = "AnchorWithoutPath", .kind = None, .anchor = L"usage", .document = kLastFile, .scroll = kRestore, .pane = kCwd },
    };
    for (const auto& c : kCases) {
        SCOPED_TRACE(c.name);
        const auto plan = DecideStartupPlan({
            .arg_kind = c.kind,
            .arg_path = c.arg,
            .anchor = c.anchor,
            .last_file = c.last_file,
            .cwd = c.cwd,
            .ignored_cwds = kIgnored,
        });
        EXPECT_EQ(plan.document_path, c.document);
        EXPECT_EQ(plan.scroll, c.scroll);
        EXPECT_EQ(plan.pane_directory, c.pane);
    }
}

// mendo.exe [パス] [--anchor 見出し]。並びは問わず、--anchor の次は - で始まっていても見出しとして読む。
// URL の書き方の # は外す。値の無い --anchor と、2 つ目以降のパスは無視する。
TEST(StartupPlanTest, ParsesPathAndAnchor)
{
    struct Case {
        const char* name;
        std::vector<const wchar_t*> argv;
        std::wstring_view path;
        std::wstring_view anchor;
    };
    const Case kCases[] = {
        { "None", {}, L"", L"" },
        { "Path", { L"a.md" }, L"a.md", L"" },
        { "PathAndAnchor", { L"a.md", L"--anchor", L"usage" }, L"a.md", L"usage" },
        { "AnchorFirst", { L"--anchor", L"usage", L"a.md" }, L"a.md", L"usage" },
        { "AnchorLooksLikeOption", { L"a.md", L"--anchor", L"--anchor" }, L"a.md", L"--anchor" },
        { "AnchorWithHash", { L"a.md", L"--anchor", L"#usage" }, L"a.md", L"usage" },
        { "AnchorOnlyHash", { L"a.md", L"--anchor", L"#" }, L"a.md", L"" },
        { "AnchorWithoutValue", { L"a.md", L"--anchor" }, L"a.md", L"" },
        { "ExtraPathIgnored", { L"a.md", L"b.md" }, L"a.md", L"" },
    };
    for (const auto& c : kCases) {
        SCOPED_TRACE(c.name);
        const auto args = ParseStartupArgs(c.argv);
        EXPECT_EQ(args.path, c.path);
        EXPECT_EQ(args.anchor, c.anchor);
    }
}

// 新しいウィンドウに渡す引数。ParseStartupArgs が読む形 (ParsesPathAndAnchor) に並べる。
TEST(StartupPlanTest, BuildsArgsForNewWindow)
{
    using Args = std::vector<std::wstring_view>;
    EXPECT_EQ(BuildStartupArgs(L"", L""), Args{});
    EXPECT_EQ(BuildStartupArgs(L"C:\\docs\\a.md", L""), (Args{ L"C:\\docs\\a.md" }));
    EXPECT_EQ(BuildStartupArgs(L"C:\\docs\\a.md", L"使い方"), (Args{ L"C:\\docs\\a.md", L"--anchor", L"使い方" }));
}
