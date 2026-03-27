#include "window.h"
#include "memory_resource.h"
#include <windows.h>
#include <shellapi.h>
#include <shellscalingapi.h>
#include <commctrl.h>

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE, LPWSTR /*lpCmdLine*/, int nCmdShow) {
    // グローバル同期プールリソースを初期化（最初に呼び出す）
    InitGlobalMemoryResource();

    // モニターごとの DPI v2 を有効化
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    // COM を初期化
    HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    if (FAILED(hr)) return 1;

    // コモンコントロールを初期化
    INITCOMMONCONTROLSEX icc{};
    icc.dwSize = sizeof(icc);
    icc.dwICC = ICC_STANDARD_CLASSES;
    InitCommonControlsEx(&icc);

    Win32Window window;
    if (!window.Create(hInstance, nCmdShow)) {
        CoUninitialize();
        return 1;
    }

    // コマンドラインでファイルが指定されていればそれを読み込み、なければ前回のファイルを復元
    // CommandLineToArgvWで正規のパースを行い、引用符やスペースを正しく処理する
    std::wstring_view initial_path;
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (argv && argc > 1) {
        initial_path = argv[1];
    }

    if (!initial_path.empty()) {
        window.LoadMarkdownFile(initial_path);
    } else {
        std::pmr::wstring last = window.LoadLastFilePath();
        if (!last.empty()) {
            window.LoadMarkdownFile(last);
        }
    }

    if (argv) {
        LocalFree(argv);
    }

    int result = window.RunMessageLoop();

    CoUninitialize();
    return result;
}
