#pragma once
#include "app.h"
#include <windows.h>
#include <string>

class Win32Window {
public:
    bool Create(HINSTANCE hInstance, int nCmdShow);
    int RunMessageLoop();

    void LoadMarkdownFile(std::wstring_view path) { app_.LoadMarkdownFile(path); }
    void LoadHelpDocument() { app_.LoadHelpDocument(); }
    std::pmr::wstring LoadLastFilePath() const { return app_.LoadLastFilePath(); }
    void ShowDirectory(std::wstring_view dir_path) { app_.ShowDirectory(dir_path); }

    // 前回セッションのスクロール位置を復元する（LoadMarkdownFileの前に呼ぶ）
    void RestoreScrollPosition();

private:
    static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
    LRESULT HandleMessage(UINT msg, WPARAM wParam, LPARAM lParam);
    LRESULT OnNcCalcSize(WPARAM wParam, LPARAM lParam);
    LRESULT OnNcHitTest(LPARAM lParam);
    void UpdateDwmFrame();
    void InitSystemMenu();
    void ResetWindowPlacement();
    void SaveWindowPlacement();
    bool RestoreWindowPlacement(int nCmdShow);

    HWND hwnd_ = nullptr;
    App app_;
};
