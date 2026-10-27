#include "theme_service.h"
#include "theme_palette.h"
#include "win32_util.h"
#include <dwrite.h>
#include <wrl/client.h>
#include <span>
#include <string>
#include <utility>

// ini 永続化キー。既存設定との互換性のため値の変更は禁止。
namespace {
using namespace std::literals;
constexpr auto kSectionView = "View"sv;
constexpr auto kKeyDarkMode = "DarkMode"sv;
constexpr auto kKeyZoomLevel = "ZoomLevel"sv;

std::wstring ResolveFontFamily(IDWriteFontCollection* collection, std::span<const wchar_t* const> candidates)
{
    if (collection) {
        for (const wchar_t* name : candidates) {
            UINT32 index = 0;
            BOOL exists = FALSE;
            if (SUCCEEDED(collection->FindFamilyName(name, &index, &exists)) && exists) {
                return name;
            }
        }
    }
    return candidates.back();
}
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

void ThemeService::Init(ZoomLevel zoom)
{
    theme_ = CreateTheme(zoom);
    // 共有ファクトリはプロセスで 1 つなので、描画側 (D2DRenderBackend) が作るものと同じになる。
    Microsoft::WRL::ComPtr<IDWriteFactory> dw;
    Microsoft::WRL::ComPtr<IDWriteFontCollection> collection;
    if (const HRESULT hr = DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory), reinterpret_cast<IUnknown**>(dw.GetAddressOf()));
        FAILED(hr)) {
        LogHrFailure("ThemeService DWriteCreateFactory", hr);
    }
    else {
        dw->GetSystemFontCollection(&collection, FALSE);
    }
    theme_.monospace_font = ResolveFontFamily(collection.Get(), theme_palette::kMonospaceFontCandidates);
    theme_.icon_font = ResolveFontFamily(collection.Get(), theme_palette::kIconFontCandidates);
}

void ThemeService::Rebuild(ZoomLevel zoom)
{
    Theme next = CreateTheme(zoom);
    next.monospace_font = std::move(theme_.monospace_font);
    next.icon_font = std::move(theme_.icon_font);
    theme_ = std::move(next);
}

bool ThemeService::ToggleDarkMode(ZoomLevel zoom)
{
    dark_mode_ = !dark_mode_;
    Rebuild(zoom);
    return dark_mode_;
}

void ThemeService::ApplyZoom(ZoomLevel zoom)
{
    Rebuild(zoom);
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
