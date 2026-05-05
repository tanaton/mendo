#include "window.h"
#include "config_service.h"
#include "i18n.h"
#include "memory_resource.h"
#include "profiler.h"
#include <windows.h>
#include <shellapi.h>
#include <shellscalingapi.h>
#include <commctrl.h>
#include <memory>

namespace {

struct LocalFreeDeleter {
    void operator()(LPWSTR* p) const noexcept
    {
        if (p) {
            LocalFree(p);
        }
    }
};

} // namespace

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE, LPWSTR /*lpCmdLine*/, int nCmdShow)
{
    MENDO_IF_TRACY(Sleep(1000)); // Tracy の起動待ち
    InitGlobalMemoryResource();

    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    const HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    if (FAILED(hr)) {
        return 1;
    }
    INITCOMMONCONTROLSEX icc{};
    icc.dwSize = sizeof(icc);
    icc.dwICC = ICC_STANDARD_CLASSES;
    InitCommonControlsEx(&icc);

    ConfigService config;
    config.Load();
    i18n::Init(config.LoadWString("General", "Language"));

    // 起動時のドキュメント決定フロー:
    //   引数が有効なファイル  → そのファイルを開く
    //   引数なし              → 前回ファイル復元、無ければヘルプ
    //   引数あり かつ無効     → 前回復元せず直接ヘルプ (ユーザの指定意図を尊重)
    // unique_ptr で管理することで、window.Create 失敗の早期 return でも LocalFree される。
    int argc = 0;
    const std::unique_ptr<LPWSTR, LocalFreeDeleter> argv_owner(CommandLineToArgvW(GetCommandLineW(), &argc));
    LPWSTR* const argv = argv_owner.get();
    const bool arg_given = argv && argc > 1 && argv[1][0] != L'\0';
    const DWORD arg_attrs = arg_given ? GetFileAttributesW(argv[1]) : INVALID_FILE_ATTRIBUTES;
    const bool has_valid_file = arg_attrs != INVALID_FILE_ATTRIBUTES && !(arg_attrs & FILE_ATTRIBUTE_DIRECTORY);

    std::pmr::wstring preload_path;
    bool restore_scroll = false;
    if (has_valid_file) {
        preload_path.assign(argv[1]);
    }
    else if (!arg_given) {
        // SessionService::LoadLastFilePath は UNC/デバイスパスや実在しないパスを除外する。
        SessionService session{ config };
        preload_path = session.LoadLastFilePath();
        if (!preload_path.empty()) {
            restore_scroll = true;
        }
    }

    int result;
    {
        Win32Window window(config);

        // ウィンドウクラス登録 + CreateWindowExW + App::Init (D3D/D2D/DWrite) と並列に
        // I/O + Markdown パースを進める。worker は App::Init 末尾で hwnd を受け取り
        // PostMessage(PARSE_COMPLETE) を発行、メッセージループ内で OnParseComplete に合流する。
        const bool has_preload = !preload_path.empty();
        if (has_preload) {
            window.StartPreloadAsync(std::move(preload_path));
        }

        {
            MENDO_PROFILE("wWinMain - Create Window");
            if (!window.Create(hInstance, nCmdShow)) {
                CoUninitialize();
                return 1;
            }
        }

        if (restore_scroll) {
            window.RestoreScrollPosition();
        }
        if (!has_preload) {
            wchar_t cwd[MAX_PATH];
            if (GetCurrentDirectoryW(MAX_PATH, cwd)) {
                window.ShowDirectory(cwd);
            }
            window.LoadHelpDocument();
        }

        result = window.RunMessageLoop();
    } // Win32Window破棄（COMオブジェクト解放）をCoUninitializeの前に完了させる

    CoUninitialize();
    return result;
}
