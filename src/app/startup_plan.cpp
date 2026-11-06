#include "startup_plan.h"
#include "file_io.h"
#include <algorithm>

namespace {

constexpr std::wstring_view kAnchorOption = L"--anchor";

bool IsIgnoredCwd(const StartupContext& ctx)
{
    const auto cwd = path_util::TrimTrailingSeparators(ctx.cwd);
    return std::ranges::any_of(ctx.ignored_cwds, [cwd](std::wstring_view dir) {
        return path_util::iequal(cwd, path_util::TrimTrailingSeparators(dir));
    });
}

// 表記の違う同じファイルでありうるか。開かずに分かる範囲で絞り、ありうるときだけ呼び出し側に開いて確かめさせる。
// ファイル名が違えば別のファイルとみなす (8.3 短縮名は ~ を含むので確かめる)。名前の違うハードリンクは見逃す。
bool MaybeSameFile(std::wstring_view a, std::wstring_view b) noexcept
{
    const auto name_a = path_util::FilenameView(a);
    const auto name_b = path_util::FilenameView(b);
    return path_util::iequal(name_a, name_b) || name_a.contains(L'~') || name_b.contains(L'~');
}

// スタートメニュー等から起動されると cwd はユーザーの作業場所と無関係になるため、
// その場合は前回ファイルのフォルダを優先する。
std::wstring PaneDirectoryFromCwd(const StartupContext& ctx)
{
    if (!ctx.last_file.empty() && (ctx.cwd.empty() || IsIgnoredCwd(ctx))) {
        return path_util::ParentDirectory(ctx.last_file);
    }
    return std::wstring{ ctx.cwd };
}

} // namespace

StartupArgs ParseStartupArgs(std::span<const wchar_t* const> argv)
{
    StartupArgs args;
    for (size_t i = 0; i < argv.size(); ++i) {
        const std::wstring_view arg = argv[i];
        if (arg == kAnchorOption) {
            if (i + 1 < argv.size()) {
                args.anchor = argv[++i];
                // URL やリンクからコピーした # 付きの書き方も受け付ける。
                if (args.anchor.starts_with(L'#')) {
                    args.anchor.remove_prefix(1);
                }
            }
        }
        else if (args.path.empty()) {
            args.path = arg;
        }
    }
    return args;
}

std::vector<std::wstring_view> BuildStartupArgs(std::wstring_view path, std::wstring_view anchor)
{
    if (path.empty()) {
        return {};
    }
    if (anchor.empty()) {
        return { path };
    }
    return { path, kAnchorOption, anchor };
}

StartupPlan DecideStartupPlan(const StartupContext& ctx)
{
    StartupPlan plan;
    switch (ctx.arg_kind) {
    case StartupArgKind::File:
        plan.document_path = ctx.arg_path;
        plan.pane_directory = path_util::ParentDirectory(ctx.arg_path);
        plan.anchor = ctx.anchor;
        if (!plan.anchor.empty()) {
            return plan;
        }
        plan.restore_scroll = path_util::iequal(ctx.arg_path, ctx.last_file);
        plan.restore_scroll_if_same_file = !plan.restore_scroll && !ctx.last_file.empty() && MaybeSameFile(ctx.arg_path, ctx.last_file);
        return plan;
    case StartupArgKind::Directory:
        plan.document_path = ctx.last_file;
        plan.pane_directory = ctx.arg_path;
        plan.restore_scroll = !ctx.last_file.empty();
        return plan;
    case StartupArgKind::None:
        plan.document_path = ctx.last_file;
        plan.pane_directory = PaneDirectoryFromCwd(ctx);
        plan.restore_scroll = !ctx.last_file.empty();
        return plan;
    case StartupArgKind::Invalid:
        // 前回ファイルは復元せず直接ヘルプを出す。
        plan.pane_directory = PaneDirectoryFromCwd(ctx);
        return plan;
    }
    std::unreachable();
}
