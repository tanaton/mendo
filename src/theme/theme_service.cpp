#include "theme_service.h"

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

Theme ThemeService::CreateTheme(ZoomLevel zoom) const
{
    Theme theme = BaseTheme();
    if (!zoom.IsDefault()) {
        theme.ApplyZoom(zoom.Value());
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

void ThemeService::SaveZoomLevel(ZoomLevel zoom)
{
    config_.SaveZoomLevel(kSectionView, kKeyZoomLevel, zoom);
}

ZoomLevel ThemeService::LoadZoomLevel() const
{
    return config_.LoadZoomLevel(kSectionView, kKeyZoomLevel);
}
