#include "theme_service.h"

ThemeService::ThemeService(ConfigService& config) noexcept
    : config_(config) {}

Theme ThemeService::CreateTheme() const {
    return dark_mode_ ? GetDarkTheme() : GetLightTheme();
}

Theme ThemeService::CreateTheme(int zoom_index) const {
    Theme theme = CreateTheme();
    if (zoom_index != ZOOM_DEFAULT_INDEX) {
        theme.ApplyZoom(ZOOM_STEPS[zoom_index]);
    }
    return theme;
}

bool ThemeService::ToggleDarkMode() {
    dark_mode_ = !dark_mode_;
    return dark_mode_;
}

void ThemeService::SaveDarkMode() {
    config_.SaveBool(L"dark_mode.txt", dark_mode_);
}

void ThemeService::LoadDarkMode() {
    dark_mode_ = config_.LoadBool(L"dark_mode.txt", false);
}

void ThemeService::SaveZoomLevel(int zoom_index) {
    config_.SaveInt(L"zoom_level.txt", zoom_index);
}

int ThemeService::LoadZoomIndex() const {
    return config_.LoadInt(L"zoom_level.txt", ZOOM_DEFAULT_INDEX, 0, ZOOM_STEP_COUNT - 1);
}
