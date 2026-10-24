#include "theme_service.h"
#include <cassert>

// ini 永続化キー。既存設定との互換性のため値の変更は禁止。
namespace {
using namespace std::literals;
constexpr auto kSectionView = "View"sv;
constexpr auto kKeyDarkMode = "DarkMode"sv;
constexpr auto kKeyZoomLevel = "ZoomLevel"sv;
} // namespace

ThemeService::ThemeService(ConfigService& config) noexcept
    : config_(config)
{
}

const Theme& ThemeService::BaseTheme() const
{
    return dark_mode_ ? GetDarkTheme() : GetLightTheme();
}

Theme ThemeService::CreateTheme(int zoom_index) const
{
    assert(zoom_index >= 0 && zoom_index < ZOOM_STEP_COUNT && "zoom_index must be within ZOOM_STEPS bounds; ZoomLevel::SetIndex clamps it");
    Theme theme = BaseTheme();
    if (zoom_index != ZOOM_DEFAULT_INDEX) {
        theme.ApplyZoom(ZOOM_STEPS[zoom_index]);
    }
    return theme;
}

bool ThemeService::ToggleDarkMode() noexcept
{
    dark_mode_ = !dark_mode_;
    return dark_mode_;
}

void ThemeService::SaveDarkMode()
{
    config_.SaveBool(kSectionView, kKeyDarkMode, dark_mode_);
}

void ThemeService::LoadDarkMode()
{
    dark_mode_ = config_.LoadBool(kSectionView, kKeyDarkMode, false);
}

void ThemeService::SaveZoomLevel(int zoom_index)
{
    config_.SaveInt(kSectionView, kKeyZoomLevel, zoom_index);
}

ZoomLevel ThemeService::LoadZoomLevel() const
{
    return config_.LoadZoomLevel(kSectionView, kKeyZoomLevel);
}
