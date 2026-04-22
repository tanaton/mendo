#include "window.h"
#include "config_store.h"
#include "i18n.h"
#include "memory_resource.h"
#include <windows.h>
#include <shellapi.h>
#include <shellscalingapi.h>
#include <commctrl.h>

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE, LPWSTR /*lpCmdLine*/, int nCmdShow)
{
    // グローバル同期プールリソースを初期化（最初に呼び出す）
    InitGlobalMemoryResource();

    // モニターごとの DPI v2 を有効化
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    // COM を初期化
    const HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    if (FAILED(hr)) {
        return 1;
    }
    // コモンコントロールを初期化
    INITCOMMONCONTROLSEX icc{};
    icc.dwSize = sizeof(icc);
    icc.dwICC = ICC_STANDARD_CLASSES;
    InitCommonControlsEx(&icc);

    config::Load();
    i18n::Init(config::GetWString("General", "Language"));

    int result;
    {
        Win32Window window;
        if (!window.Create(hInstance, nCmdShow)) {
            CoUninitialize();
            return 1;
        }

        // 起動時のドキュメント決定フロー:
        //   引数が有効なファイル  → そのファイルを開く
        //   引数なし              → 前回ファイル復元、無ければヘルプ
        //   引数あり かつ無効     → 前回復元せず直接ヘルプ (ユーザの指定意図を尊重)
        // CommandLineToArgvWで正規のパースを行い、引用符やスペースを正しく処理する。
        // 引数が --version 等のフラグ文字列でも確実にヘルプへ落とすため
        // GetFileAttributesW で実在を検証する。
        // ディレクトリが渡されても「有効なファイル」扱いしないよう属性ビットで除外。
        int argc = 0;
        LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
        const bool arg_given = argv && argc > 1 && argv[1][0] != L'\0';
        const DWORD arg_attrs = arg_given ? GetFileAttributesW(argv[1]) : INVALID_FILE_ATTRIBUTES;
        const bool has_valid_file = arg_attrs != INVALID_FILE_ATTRIBUTES
            && !(arg_attrs & FILE_ATTRIBUTE_DIRECTORY);

        if (has_valid_file) {
            window.LoadMarkdownFile(argv[1]);
        }
        else {
            std::pmr::wstring last;
            if (!arg_given) {
                last = window.LoadLastFilePath();
            }
            if (!last.empty()) {
                window.RestoreScrollPosition();
                window.LoadMarkdownFile(last);
            }
            else {
                wchar_t cwd[MAX_PATH];
                if (GetCurrentDirectoryW(MAX_PATH, cwd)) {
                    window.ShowDirectory(cwd);
                }
                window.LoadHelpDocument();
            }
        }

        if (argv) {
            LocalFree(argv);
        }

        result = window.RunMessageLoop();
    } // Win32Window破棄（COMオブジェクト解放）をCoUninitializeの前に完了させる

    CoUninitialize();
    return result;
}
