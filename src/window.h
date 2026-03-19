#pragma once
#include "app.h"
#include <windows.h>
#include <string>

class Win32Window {
public:
    bool Create(HINSTANCE hInstance, int nCmdShow);
    int RunMessageLoop();

    void LoadMarkdownFile(const std::wstring& path) { app_.LoadMarkdownFile(path); }
    std::wstring LoadLastFilePath() const { return app_.LoadLastFilePath(); }

private:
    static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
    LRESULT HandleMessage(UINT msg, WPARAM wParam, LPARAM lParam);

    HWND hwnd_ = nullptr;
    App app_;
};
