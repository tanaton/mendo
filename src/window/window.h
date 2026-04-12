#pragma once
#include <windows.h>
#include <string>
#include <string_view>
#include <memory>
#include <memory_resource>

class App;

class Win32Window {
public:
    Win32Window();
    ~Win32Window();

    bool Create(HINSTANCE hInstance, int nCmdShow);
    int RunMessageLoop();

    void LoadMarkdownFile(std::wstring_view path);
    void LoadHelpDocument();
    std::pmr::wstring LoadLastFilePath() const;
    void ShowDirectory(std::wstring_view dir_path);

    // 前回セッションのスクロール位置を復元する（LoadMarkdownFileの前に呼ぶ）
    void RestoreScrollPosition();

private:
    static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
    LRESULT HandleMessage(UINT msg, WPARAM wParam, LPARAM lParam);
    LRESULT OnNcCalcSize(WPARAM wParam, LPARAM lParam);
    LRESULT OnNcHitTest(LPARAM lParam);
    LRESULT HandleMouseMessage(UINT msg, WPARAM wParam, LPARAM lParam);
    LRESULT HandleAppNotification(UINT msg, WPARAM wParam, LPARAM lParam);
    void UpdateDwmFrame();
    void InitSystemMenu();
    void ResetWindowPlacement();
    void SaveWindowPlacement();
    bool RestoreWindowPlacement(int nCmdShow);

    static LRESULT CALLBACK SearchEditProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam,
        UINT_PTR uIdSubclass, DWORD_PTR dwRefData);
    void RepositionSearchEdit();
    void SyncSearchCaretFromEdit();

    void UpdateDpiMetricsCache();

    HWND hwnd_ = nullptr;
    HWND search_edit_ = nullptr;
    bool in_sys_menu_ = false;   // システムメニューのモーダルループ中フラグ
    bool tracking_mouse_ = false; // TrackMouseEvent によるマウス追跡中フラグ

    // WM_NCHITTEST用DPIメトリクスキャッシュ（WM_DPICHANGED時に更新）
    int cached_nchit_border_ = 4;
    int cached_nchit_frame_y_ = 8;
    int cached_nchit_right_border_ = 8;

    std::unique_ptr<App> app_;
};
