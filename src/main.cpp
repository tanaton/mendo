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
    // Tracy プロファイラ接続を待つ初期遅延 (ms)。1 秒は Tracy GUI が profile-side に
    // attach するのに十分な時間で、Release ビルドでは MENDO_IF_TRACY が空展開される。
    [[maybe_unused]] constexpr DWORD kTracyStartupDelayMs = 1000;
    MENDO_IF_TRACY(Sleep(kTracyStartupDelayMs));
    MENDO_FRAME_MARK();

    {
        MENDO_PROFILE("wWinMain - init");
        // 遅延ロード DLL と LoadLibrary を exe のフォルダやカレントディレクトリ (文書のフォルダ) から探させない。
        SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_SYSTEM32);
        // メディアの無いドライブを指す画像パスで「ディスクがありません」ダイアログを出させない。
        SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOOPENFILEERRORBOX);
        // DPI 認識 (PerMonitorV2) はマニフェストで宣言しているので、ここでは設定しない。
        // 遅延ロードの D2D / DWrite を、D3D 先行作成スレッドの GPU ドライバ読み込み (ローダーロック) と
        // 重ねてメインスレッドが待たないよう、そのスレッドを始める前に読んでおく。
        LoadLibraryExW(L"d2d1.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        LoadLibraryExW(L"dwrite.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    }

    PrefetchD3DDevice();

    // CoUninitialize は Win32Window 破棄（COM オブジェクト解放）より後に走らせる必要がある。
    // window より先にこのガードを宣言することで、全 return 経路でこの順序を保証する。
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

    // 引数は CRT がスタートアップで分解済み。
    const StartupArgs args = (__wargv && __argc > 1) ? ParseStartupArgs({ __wargv + 1, static_cast<size_t>(__argc - 1) }) : StartupArgs{};

    // session::LoadLastFilePath は UNC/デバイスパスや実在しないパスを除外する (ネットワークドライブは確かめない)。
    const std::wstring last_file = session::LoadLastFilePath(config);
    StartupPlan plan = PlanStartup(args, last_file);

    Win32Window window(config);

    // ウィンドウクラス登録 + CreateWindowExW + App::Init (D3D/D2D/DWrite) と並列に
    // I/O + Markdown パースを進める。worker は Create 末尾で hwnd を受け取り
    // ::PostMessageW(PARSE_COMPLETE) を発行、メッセージループ内で OnParseComplete に合流する。
    // preload が無ければ Create 末尾でヘルプを開く。
    if (!plan.document_path.empty()) {
        window.app().StartPreloadAsync(std::move(plan.document_path));
    }

    // preload が Create 内で同期完了するパスに備え、復元情報を先にセット。
    std::error_code ec;
    if (plan.restore_scroll || (plan.restore_scroll_if_same_file && std::filesystem::equivalent(args.path, last_file, ec))) {
        window.app().RestoreScrollPosition();
    }
    if (!plan.anchor.empty()) {
        window.app().SetInitialAnchor(plan.anchor);
    }
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
