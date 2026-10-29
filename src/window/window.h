#pragma once
#include "app.h"
#include <windows.h>
#include <memory>
#include <optional>

class ConfigService;

class Win32Window final : public WindowHost {
public:
    explicit Win32Window(ConfigService& config);
    ~Win32Window();

    bool Create(HINSTANCE hInstance, int nCmdShow);
    int RunMessageLoop();
    void RunUntil(const bool& done) override;
    void FocusSearchEdit(int anchor, int caret) override;
    void UnfocusSearch(bool clear_text) override;

    // 起動時の準備 (preload・スクロール位置・ファイルペインのフォルダ) は Create より前に App へ直接頼む。
    App& app() noexcept
    {
        return *app_;
    }

private:
    // WM_QUIT を受けたら終了コードを、done が立ったら nullopt を返す。
    std::optional<int> PumpMessages(const bool& done);
    static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
    LRESULT HandleMessage(UINT msg, WPARAM wParam, LPARAM lParam);
    LRESULT OnNcCalcSize(WPARAM wParam, LPARAM lParam);
    // スクリーン座標 pt での WM_NCHITTEST の結果。
    LRESULT HitTestNonClient(POINT pt);
    // 最大化時は HTNOWHERE を返す。
    LRESULT HitTestResizeFrame(POINT pt) const noexcept;
    LRESULT HitTestTitleBar(POINT pt) const noexcept;
    // マウス系のメッセージでなければ nullopt。
    std::optional<LRESULT> TryHandleMouseMessage(UINT msg, WPARAM wParam, LPARAM lParam);
    void OnContextMenu(int screen_x, int screen_y);
    POINT KeyboardContextMenuPoint() const;
    void OnSize(WPARAM wParam, LPARAM lParam);
    void UpdateDwmFrame();
    void InitSystemMenu();
    void ResetWindowPlacement();
    void SaveWindowPlacement();
    bool RestoreWindowPlacement(int nCmdShow);

    static LRESULT CALLBACK SearchEditProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam, UINT_PTR uIdSubclass, DWORD_PTR dwRefData);
    LRESULT OnSearchEditKeyDown(HWND edit, UINT msg, WPARAM wParam, LPARAM lParam);
    void OnSearchEditChanged();
    bool EnsureSearchEdit();
    // EDIT は見えないので、置き場所は IME の変換窓の位置にしか効かない。
    void RepositionSearchEdit();
    void SyncSearchCaretFromEdit();

    void UpdateDpiMetricsCache();
    void AwaitInitialParseBeforeDevice();
    // 起動時の、デバイスに依存しない通知を最初のフレームより前に取り込む。デバイスの作成を待つ前に呼ぶと、
    // ファイルペインの一覧の行の文字レイアウト (CreateRenderDevice の PrepareChrome) と、ディスクキャッシュにある
    // インライン数式の読み込みを作成と並行して進められる。待つ間に届いた分のため、作成の後にも呼ぶ。
    void TakeStartupMessages();

    HWND hwnd_ = nullptr;
    HWND search_edit_ = nullptr;
    bool tracking_mouse_ = false; // TME_LEAVE は WM_MOUSELEAVE を 1 回送ると解除されるので、再登録の要否を覚える
    bool was_minimized_ = false;  // 直前が最小化状態だったかのフラグ（復元時の同期描画ガード用）

    // WM_DPICHANGED 時に更新。
    int cached_nchit_border_ = 4;
    int cached_nchit_frame_y_ = 8;
    int cached_nchit_right_border_ = 8;

    ConfigService& config_;
    std::unique_ptr<App> app_;
};
