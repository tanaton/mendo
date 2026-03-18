#include "window.h"
#include <windows.h>
#include <shellscalingapi.h>
#include <commctrl.h>

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE, LPWSTR lpCmdLine, int nCmdShow) {
    // Enable Per-Monitor DPI v2
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    // Initialize COM
    HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    if (FAILED(hr)) return 1;

    // Initialize common controls
    INITCOMMONCONTROLSEX icc{};
    icc.dwSize = sizeof(icc);
    icc.dwICC = ICC_STANDARD_CLASSES;
    InitCommonControlsEx(&icc);

    MainWindow window;
    if (!window.Create(hInstance, nCmdShow)) {
        CoUninitialize();
        return 1;
    }

    // Load file from command line if provided, otherwise restore last file
    if (lpCmdLine && lpCmdLine[0]) {
        std::wstring path = lpCmdLine;
        // Strip quotes if present
        if (path.size() >= 2 && path.front() == L'"' && path.back() == L'"') {
            path = path.substr(1, path.size() - 2);
        }
        window.LoadMarkdownFile(path);
    } else {
        std::wstring last = MainWindow::LoadLastFilePath();
        if (!last.empty()) {
            window.LoadMarkdownFile(last);
        }
    }

    int result = window.RunMessageLoop();

    CoUninitialize();
    return result;
}
