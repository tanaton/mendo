#include "startup_plan.h"
#include "file_io.h"
#include <algorithm>
#include <filesystem>

namespace {

std::pmr::wstring ParentDirectory(std::wstring_view path)
{
    return std::pmr::wstring{ std::filesystem::path(path).parent_path().native() };
}

bool IsIgnoredCwd(const StartupContext& ctx)
{
    const auto cwd = path_util::TrimTrailingSeparators(ctx.cwd);
    return std::ranges::any_of(ctx.ignored_cwds, [cwd](std::wstring_view dir) {
        return path_util::iequal(cwd, path_util::TrimTrailingSeparators(dir));
    });
}

// スタートメニュー等から起動されると cwd はユーザーの作業場所と無関係になるため、
// その場合は前回ファイルのフォルダを優先する。
std::pmr::wstring PaneDirectoryFromCwd(const StartupContext& ctx)
{
    if (!ctx.last_file.empty() && (ctx.cwd.empty() || IsIgnoredCwd(ctx))) {
        return ParentDirectory(ctx.last_file);
    }
    return std::pmr::wstring{ ctx.cwd };
}

} // namespace

StartupPlan DecideStartupPlan(const StartupContext& ctx)
{
    StartupPlan plan;
    switch (ctx.arg_kind) {
    case StartupArgKind::File:
        plan.document_path = ctx.arg_path;
        plan.restore_scroll = ctx.arg_is_last_file;
        plan.pane_directory = ParentDirectory(ctx.arg_path);
        break;
    case StartupArgKind::Directory:
        plan.document_path = ctx.last_file;
        plan.restore_scroll = !ctx.last_file.empty();
        plan.pane_directory = ctx.arg_path;
        break;
    case StartupArgKind::None:
        plan.document_path = ctx.last_file;
        plan.restore_scroll = !ctx.last_file.empty();
        plan.pane_directory = PaneDirectoryFromCwd(ctx);
        break;
    case StartupArgKind::Invalid:
        // 前回ファイルは復元せず直接ヘルプを出す。
        plan.pane_directory = PaneDirectoryFromCwd(ctx);
        break;
    }
    return plan;
}
