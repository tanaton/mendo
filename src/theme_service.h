#pragma once
#include "config_service.h"
#include "theme.h"
#include <string>

// Manages dark mode state, theme creation, and related persistence.
// No Win32 dependency — fully testable.
class ThemeService {
public:
    explicit ThemeService(ConfigService& config) noexcept;

    // ---- State ----

    bool IsDarkMode() const noexcept { return dark_mode_; }

    // Create a theme for the current dark mode state with optional zoom applied.
    Theme CreateTheme() const;
    Theme CreateTheme(int zoom_index) const;

    // Toggle dark mode. Returns the new dark_mode state.
    bool ToggleDarkMode();

    // ---- Persistence ----

    void SaveDarkMode();
    void LoadDarkMode();

    void SaveZoomLevel(int zoom_index);
    int LoadZoomIndex() const;

private:
    ConfigService& config_;
    bool dark_mode_ = false;
};
