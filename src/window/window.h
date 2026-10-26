#pragma once
#include "ui_types.h"
#include <windows.h>
#include <string>
#include <string_view>
#include <memory>
#include <memory_resource>
#include <optional>

class App;
class ConfigService;

class Win32Window final : public ModalLoop {
public:
    explicit Win32Window(ConfigService& config);
    ~Win32Window();

    bool Create(HINSTANCE hInstance, int nCmdShow);
    int RunMessageLoop();
    void RunUntil(const bool& done) override;

    void SetInitialDirectory(std::wstring_view dir_path);
    void StartPreloadAsync(std::pmr::wstring path);

    // App::LoadInitialDocument が FileLoadService::AttachPreload で同期復元するパスに備えて、Create より前に呼ぶ。
    void RestoreScrollPosition();

private:
    // WM_QUIT を受けたら終了コードを、done が立ったら nullopt を返す。
    std::optional<int> PumpMessages(const bool& done);
    static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
    LRESULT HandleMessage(UINT msg, WPARAM wParam, LPARAM lParam);
    LRESULT OnNcCalcSize(WPARAM wParam, LPARAM lParam);
    LRESULT OnNcHitTest(LPARAM lParam);
    // 最大化時は HTNOWHERE を返す。
    LRESULT HitTestResizeFrame(POINT pt) const noexcept;
    LRESULT HitTestTitleBar(POINT pt) const noexcept;
    // マウス系のメッセージでなければ nullopt。
    std::optional<LRESULT> TryHandleMouseMessage(UINT msg, WPARAM wParam, LPARAM lParam);
    LRESULT HandleAppNotification(UINT msg, WPARAM wParam, LPARAM lParam);
    void OnContextMenu(int screen_x, int screen_y);
    POINT KeyboardContextMenuPoint() const;
    void OnSize(WPARAM wParam, LPARAM lParam);
    void UpdateDwmFrame();
    void InitSystemMenu();
    void ResetWindowPlacement();
    void SaveWindowPlacement();
    bool RestoreWindowPlacement(int nCmdShow);

    static LRESULT CALLBACK SearchEditProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam, UINT_PTR uIdSubclass, DWORD_PTR dwRefData);
    LRESULT OnSearchEditKeyDown(HWND edit, WPARAM wParam, LPARAM lParam);
    void OnSearchEditChanged();
    bool EnsureSearchEdit();
    // selection は app_param::MakeSearchSelectionLParam で詰めた [anchor, caret)。
    void FocusSearchEdit(LPARAM selection);
    void RepositionSearchEdit();
    void SyncSearchCaretFromEdit();

    void UpdateDpiMetricsCache();
    void AwaitInitialParseBeforeDevice();

    HWND hwnd_ = nullptr;
    HWND search_edit_ = nullptr;
    bool in_sys_menu_ = false;
    bool tracking_mouse_ = false; // TME_LEAVE は WM_MOUSELEAVE を 1 回送ると解除されるので、再登録の要否を覚える
    bool was_minimized_ = false;  // 直前が最小化状態だったかのフラグ（復元時の同期描画ガード用）

    std::pmr::wstring search_text_buf_;

    // WM_DPICHANGED 時に更新。
    int cached_nchit_border_ = 4;
    int cached_nchit_frame_y_ = 8;
    int cached_nchit_right_border_ = 8;

    ConfigService& config_;
    std::unique_ptr<App> app_;
};
