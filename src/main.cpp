#include "window.h"
#include "config_service.h"
#include "file_io.h"
#include "i18n.h"
#include "profiler.h"
#include "scope_guard.h"
#include "startup_plan.h"
#include <windows.h>
#include <shellapi.h>
#include <commctrl.h>
#include <array>
#include <filesystem>
#include <string>
#include <system_error>

namespace {

template <typename GetDirFn>
std::wstring QueryDirectory(GetDirFn get_dir)
{
    wchar_t buf[MAX_PATH];
    const UINT len = get_dir(buf, MAX_PATH);
    return (len > 0 && len < MAX_PATH) ? std::wstring{ buf, len } : std::wstring{};
}

std::wstring ExeDirectory()
{
    wchar_t buf[MAX_PATH];
    const DWORD len = GetModuleFileNameW(nullptr, buf, MAX_PATH);
    if (len == 0 || len >= MAX_PATH) {
        return {};
    }
    const std::wstring_view exe{ buf, len };
    return std::wstring{ exe.substr(0, exe.find_last_of(L'\\')) };
}

// "." や相対パスを FileExplorer / 前回ファイル比較でそのまま扱えるよう絶対パス化する。
// MSVC の absolute は GetFullPathNameW 経由で "." / ".." も解決する。
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

// 大文字小文字・スラッシュ違いを吸収するため filesystem::equivalent でも比較する。
bool IsSameFile(std::wstring_view a, std::wstring_view b)
{
    std::error_code ec;
    return path_util::iequal(a, b) || std::filesystem::equivalent(a, b, ec);
}

StartupPlan PlanStartup(std::wstring_view arg, std::wstring_view last_file)
{
    const std::wstring cwd = QueryDirectory([](wchar_t* b, UINT n) { return GetCurrentDirectoryW(n, b); });
    const std::array<std::wstring, 3> ignored_cwds = {
        QueryDirectory(GetSystemDirectoryW),
        QueryDirectory(GetWindowsDirectoryW),
        ExeDirectory(),
    };
    const std::array<std::wstring_view, 3> ignored_cwd_views = { ignored_cwds[0], ignored_cwds[1], ignored_cwds[2] };
    StartupContext ctx{
        .last_file = last_file,
        .cwd = cwd,
        .ignored_cwds = ignored_cwd_views,
    };

    std::wstring arg_path;
    if (!arg.empty()) {
        arg_path = ToAbsolutePath(arg);
        ctx.arg_path = arg_path;
        ctx.arg_kind = ClassifyPath(arg_path);
        ctx.arg_is_last_file = ctx.arg_kind == StartupArgKind::File && !last_file.empty() && IsSameFile(arg_path, last_file);
    }
    return DecideStartupPlan(ctx);
}

} // namespace

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE, LPWSTR /*lpCmdLine*/, int nCmdShow)
{
    // Tracy プロファイラ接続を待つ初期遅延 (ms)。1 秒は Tracy GUI が profile-side に
    // attach するのに十分な経験値で、Release ビルドでは MENDO_IF_TRACY が空展開される。
    [[maybe_unused]] constexpr DWORD kTracyStartupDelayMs = 1000;
    MENDO_IF_TRACY(Sleep(kTracyStartupDelayMs));

    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    const HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    if (FAILED(hr)) {
        return 1;
    }
    // CoUninitialize は Win32Window 破棄（COM オブジェクト解放）より後に走らせる必要がある。
    // window より先にこのガードを宣言することで、全 return 経路でこの順序を保証する。
    auto com_guard = ScopeGuard([] { CoUninitialize(); });
    INITCOMMONCONTROLSEX icc{};
    icc.dwSize = sizeof(icc);
    icc.dwICC = ICC_STANDARD_CLASSES;
    InitCommonControlsEx(&icc);

    ConfigService config;
    config.Load();
    i18n::Init(config.LoadWString("General", "Language"));

    int argc = 0;
    LPWSTR* const argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    auto argv_guard = ScopeGuard([argv] { LocalFree(argv); });
    const std::wstring_view arg = (argv && argc > 1) ? std::wstring_view{ argv[1] } : std::wstring_view{};

    // SessionService::LoadLastFilePath は UNC/デバイスパスや実在しないパスを除外する。
    SessionService session{ config };
    const auto last_file = session.LoadLastFilePath();
    StartupPlan plan = PlanStartup(arg, last_file);

    Win32Window window(config);

    // ウィンドウクラス登録 + CreateWindowExW + App::Init (D3D/D2D/DWrite) と並列に
    // I/O + Markdown パースを進める。worker は App::Init 末尾で hwnd を受け取り
    // ::PostMessageW(PARSE_COMPLETE) を発行、メッセージループ内で OnParseComplete に合流する。
    const bool has_preload = !plan.document_path.empty();
    if (has_preload) {
        window.StartPreloadAsync(std::move(plan.document_path));
    }

    // preload が Create 内 (App::Init) で同期完了するパスに備え、復元情報を先にセット。
    if (plan.restore_scroll) {
        window.RestoreScrollPosition();
    }
    // Create 末尾の初回描画に間に合わせる。
    if (!plan.pane_directory.empty()) {
        window.SetInitialDirectory(plan.pane_directory);
    }

    {
        MENDO_PROFILE("wWinMain - Create Window");
        if (!window.Create(hInstance, nCmdShow)) {
            return 1;
        }
    }

    if (!has_preload) {
        window.LoadHelpDocument();
    }

    return window.RunMessageLoop();
}
