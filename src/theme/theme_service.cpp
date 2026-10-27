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

const Theme& BaseTheme(bool dark)
{
    return dark ? GetDarkTheme() : GetLightTheme();
}

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

Theme ThemeService::MakeTheme(bool dark, ZoomLevel zoom)
{
    Theme theme = BaseTheme(dark);
    if (!zoom.IsDefault()) {
        theme.ApplyZoom(zoom.Value());
    }
    return theme;
}

Theme ThemeService::CreateTheme(ZoomLevel zoom) const
{
    return MakeTheme(theme_.dark, zoom);
}

void ThemeService::Init(bool dark, ZoomLevel zoom)
{
    theme_ = MakeTheme(dark, zoom);
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

// ライトとダークの違いは色だけ。
bool ThemeService::ToggleDarkMode()
{
    const bool dark = !theme_.dark;
    theme_.colors = BaseTheme(dark).colors;
    theme_.dark = dark;
    return dark;
}

// 累積の誤差を溜めないよう、毎回ベーステーマから作る。決めたフォントは引き継ぐ。
void ThemeService::ApplyZoom(ZoomLevel zoom)
{
    Theme next = CreateTheme(zoom);
    next.monospace_font = std::move(theme_.monospace_font);
    next.icon_font = std::move(theme_.icon_font);
    theme_ = std::move(next);
}

void ThemeService::SaveDarkMode()
{
    config_.SaveBool(kSectionView, kKeyDarkMode, theme_.dark);
}

bool ThemeService::LoadDarkMode() const
{
    return config_.LoadBool(kSectionView, kKeyDarkMode, false);
}

void ThemeService::SaveZoomLevel(ZoomLevel zoom)
{
    config_.SaveZoomLevel(kSectionView, kKeyZoomLevel, zoom);
}

ZoomLevel ThemeService::LoadZoomLevel() const
{
    return config_.LoadZoomLevel(kSectionView, kKeyZoomLevel);
}
