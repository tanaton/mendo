#pragma once
#include "tooltip_target.h"
#include <memory>

// Win32 TOOLTIPS_CLASS を TTF_TRACK モードで管理するラッパー。
// App 側のホバー検出結果をもとに、ツールチップの表示/非表示を制御する。
//
// PIMPL 化によりヘッダから <windows.h> / <commctrl.h> を追い出し、
// app_state.h → tooltip.h 経由での Win32 依存波及を遮断している。
// 実装は tooltip.cpp に全て閉じ込める。
class Tooltip {
public:
    Tooltip();
    ~Tooltip();
    Tooltip(const Tooltip&) = delete;
    Tooltip& operator=(const Tooltip&) = delete;
    Tooltip(Tooltip&&) noexcept;
    Tooltip& operator=(Tooltip&&) noexcept;

    // 親ウィンドウ（HWND）を void* で受け取り、ツールチップを作成する。
    void Init(void* parent_hwnd);

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
