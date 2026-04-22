#pragma once
#include "config_service.h"
#include "theme.h"
#include <string>

// ダークモード状態、テーマ作成、および関連する永続化を管理する。
// Win32依存なし — 完全にテスト可能。
class ThemeService {
public:
    explicit ThemeService(ConfigService& config) noexcept;

    // ---- 状態 ----

    constexpr bool IsDarkMode() const noexcept { return dark_mode_; }

    Theme CreateTheme() const;
    Theme CreateTheme(int zoom_index) const;
    bool ToggleDarkMode() noexcept;

    // ---- 永続化 ----

    void SaveDarkMode();
    void LoadDarkMode();

    void SaveZoomLevel(int zoom_index);
    int LoadZoomIndex() const;

private:
    ConfigService& config_;
    bool dark_mode_ = false;
};
