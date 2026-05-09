#pragma once
#include "app_events.h"
#include "titlebar.h"
#include <windows.h>
#include <functional>

struct AppState;

// タイトルバー周りの責務 (高さ取得・ヒットテスト・無効化・クリック処理・タイトル更新)を集約する。
// state_.window.titlebar / pane_layout_cache の幅 / DPI scale / hwnd を見て描画通知を出す。
// クリックで発火する AppAction の配信は dispatch コールバック、 WM_CLOSE のような
// 直接の Win32 メッセージ送出は post_window_message コールバックを通す。
class TitleBarController {
public:
    // ガードを書かなくて済むようデフォルトは no-op。Init で必ず差し替えること。
    struct Callbacks {
        std::function<void(const AppAction&)> dispatch = [](const AppAction&) {};
        std::function<void(UINT msg, WPARAM, LPARAM)> post_window_message = [](UINT, WPARAM, LPARAM) {};
    };

    void Init(HWND hwnd, AppState* state, Callbacks callbacks) noexcept;

    float GetHeightDip() const noexcept;
    TitleBarHitZone HitTest(float dip_x, float dip_y) const noexcept;
    void Invalidate() const noexcept;
    // クリックがタイトルバー内なら true を返し、 内部で対応するアクションを発行する。
    bool HandleClick(float dip_x, float dip_y);
    // ズーム % + ファイル名でタイトル文字列を再構築し、 描画通知も発行する。
    void Update();

private:
    HWND hwnd_ = nullptr;
    AppState* state_ = nullptr;
    Callbacks callbacks_;
};
