#include "window.h"
#include "app.h"
#include "config_service.h"
#include "renderer.h"
#include "file_io.h"
#include "i18n.h"
#include "profiler.h"
#include "startup_plan.h"
#include "utility.h"
#include "win32_util.h"
#include <windows.h>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <system_error>

namespace {

std::wstring ToAbsolutePath(std::wstring_view path)
{
    std::error_code ec;
    const auto abs = std::filesystem::absolute(path, ec);
    return ec ? std::wstring{ path } : abs.native();
}

StartupArgKind ClassifyPath(const std::wstring& path)
{
    const DWORD attrs = GetFileAttributesW(path.c_str());
    if (attrs == INVALID_FILE_ATTRIBUTES) {
        return StartupArgKind::Invalid;
    }
    return (attrs & FILE_ATTRIBUTE_DIRECTORY) ? StartupArgKind::Directory : StartupArgKind::File;
}

StartupPlan PlanStartup(const StartupArgs& args, std::wstring_view last_file)
{
    MENDO_PROFILE("PlanStartup");
    const std::wstring cwd = QueryPath([](wchar_t* b, UINT n) static { return GetCurrentDirectoryW(n, b); });
    const std::wstring ignored_cwds[]{
        QueryPath(GetSystemDirectoryW),
        QueryPath(GetWindowsDirectoryW),
        path_util::ParentDirectory(ExePath()),
    };
    const std::wstring_view ignored_cwd_views[]{ ignored_cwds[0], ignored_cwds[1], ignored_cwds[2] };
    StartupContext ctx{
        .anchor = args.anchor,
        .last_file = last_file,
        .cwd = cwd,
        .ignored_cwds = ignored_cwd_views,
    };

    std::wstring arg_path;
    if (!args.path.empty()) {
        arg_path = ToAbsolutePath(args.path);
        ctx.arg_path = arg_path;
        ctx.arg_kind = ClassifyPath(arg_path);
    }
    return DecideStartupPlan(ctx);
}

} // namespace

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE, LPWSTR /*lpCmdLine*/, int nCmdShow)
{
    // 起動の計測を取りこぼさないよう、Tracy の GUI の接続を待つ
    [[maybe_unused]] constexpr DWORD kTracyStartupDelayMs = 1000;
    MENDO_IF_TRACY(Sleep(kTracyStartupDelayMs));
    MENDO_FRAME_MARK();

    {
        MENDO_PROFILE("wWinMain - init");
        SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_SYSTEM32);
        SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOOPENFILEERRORBOX);
        LoadLibraryExW(L"d2d1.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        LoadLibraryExW(L"dwrite.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    }

    PrefetchD3DDevice();

    bool com_initialized = false;
    ScopeGuard com_guard([&com_initialized] {
        if (com_initialized) {
            CoUninitialize();
        }
    });

    ConfigService config;
    {
        MENDO_PROFILE("wWinMain - config");
        config.Load();
        i18n::Init(session::LoadLanguage(config));
    }

    const StartupArgs args = (__wargv && __argc > 1) ? ParseStartupArgs({ __wargv + 1, static_cast<size_t>(__argc - 1) }) : StartupArgs{};
    const std::wstring last_file = session::LoadLastFilePath(config);
    StartupPlan plan = PlanStartup(args, last_file);

    Win32Window window(config);

    if (!plan.document_path.empty()) {
        window.app().StartPreloadAsync(std::move(plan.document_path));
    }

    // preload の結果は Create の中で取り込むので、復元情報を先に渡す。
    // clang-format off
    std::visit(overloaded{
        [](const startup_scroll::None&) static {},
        [&window](const startup_scroll::Restore&) { window.app().RestoreScrollPosition(); },
        [&](const startup_scroll::RestoreIfSameFile&) {
            std::error_code ec;
            if (!path_util::IsOnNetworkDrive(last_file) && std::filesystem::equivalent(args.path, last_file, ec)) {
                window.app().RestoreScrollPosition();
            }
        },
        [&window](const startup_scroll::Anchor& a) { window.app().SetInitialAnchor(a.name); },
    }, plan.scroll);
    // clang-format on
    // Create 末尾の初回描画に間に合わせる。
    if (!plan.pane_directory.empty()) {
        window.app().SetInitialDirectory(plan.pane_directory);
    }

    // COM を使うのは Create (App::Init の D2D / DWrite / WIC) 以降だけなので、
    // ここまで遅らせて preload を先に走らせる。worker は COM が要るときだけ自分で MTA に入る。
    if (FAILED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE))) {
        return 1;
    }
    com_initialized = true;

    if (!window.Create(hInstance, nCmdShow)) {
        return 1;
    }

    return window.RunMessageLoop();
}
