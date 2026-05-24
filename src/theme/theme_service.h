#pragma once
#include "config_service.h"
#include "theme.h"
#include <string>

class ThemeService {
public:
    explicit ThemeService(ConfigService& config) noexcept;

    constexpr bool IsDarkMode() const noexcept
    {
        return dark_mode_;
    }

    Theme CreateTheme() const;
    Theme CreateTheme(int zoom_index) const;
    bool ToggleDarkMode() noexcept;

    void SaveDarkMode();
    void LoadDarkMode();

    void SaveZoomLevel(int zoom_index);
    int LoadZoomIndex() const;

private:
    ConfigService& config_;
    bool dark_mode_ = false;
};
