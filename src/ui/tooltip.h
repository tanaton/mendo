#pragma once
#include "tooltip_target.h"
#include <memory>

// <windows.h> を巻き込まずに HWND を扱うための前方宣言（Windows SDK の
// DECLARE_HANDLE(HWND) と ABI 互換）。
struct HWND__;
using HWND = HWND__*;

// App 側のホバー検出結果をもとに、ツールチップの表示/非表示を制御する。
class Tooltip {
public:
    Tooltip();
    ~Tooltip();
    Tooltip(const Tooltip&) = delete;
    Tooltip& operator=(const Tooltip&) = delete;
    Tooltip(Tooltip&&) noexcept;
    Tooltip& operator=(Tooltip&&) noexcept;

    // 親ウィンドウを受け取り、ツールチップを作成する。
    void Init(HWND parent_hwnd);

    // ホバー対象が変わったらタイマーのリセットが必要かを返す。
    // screen_x / screen_y: マウスのスクリーン座標（ツールチップ表示位置用）。
    // 戻り値: タイマーを再設定すべきなら true。
    bool Update(const TooltipTarget& target, int screen_x, int screen_y);

    void Show();
    void Hide();
    void ApplyDarkMode(bool dark);
    void ResetTarget() noexcept;

    const TooltipTarget& GetCurrent() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
