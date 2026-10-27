#pragma once
#include "config_service.h"
#include "theme.h"

class ThemeService {
public:
    explicit ThemeService(ConfigService& config) noexcept;

    constexpr bool IsDarkMode() const noexcept
    {
        return dark_mode_;
    }

    // 今のテーマ。描画・計測・ポップアップはこれを参照で持つ。ダークモードとズームの切り替えは
    // 同じ場所を書き換えるので、アドレスは変わらない。
    constexpr const Theme& GetTheme() const noexcept
    {
        return theme_;
    }

    const Theme& BaseTheme() const;
    // ベーステーマに zoom を掛けたもの。フォントは候補の先頭のまま (起動時の先読みの推定はフォント名を使わない)。
    Theme CreateTheme(ZoomLevel zoom) const;

    // 今のテーマを zoom で作り、フォントを実在する候補に決める。フォントはテーマ間で共通なので、決めるのはここだけ。
    void Init(ZoomLevel zoom);
    // 色だけが変わる。寸法とフォントはそのまま (zoom は今の倍率)。
    bool ToggleDarkMode(ZoomLevel zoom);
    // 寸法が変わる。色とフォントはそのまま。
    void ApplyZoom(ZoomLevel zoom);

    void SaveDarkMode();
    void LoadDarkMode();

    void SaveZoomLevel(ZoomLevel zoom);
    ZoomLevel LoadZoomLevel() const;

private:
    // ベーステーマに zoom を掛けて今のテーマにする。累積の誤差を溜めないよう、毎回ベーステーマから作る。
    // 決めたフォントは引き継ぐ。
    void Rebuild(ZoomLevel zoom);

    ConfigService& config_;
    bool dark_mode_ = false;
    Theme theme_;
};
