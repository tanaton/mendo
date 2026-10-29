#pragma once
#include "config_service.h"
#include "theme.h"

class ThemeService {
public:
    explicit ThemeService(ConfigService& config) noexcept;

    constexpr bool IsDarkMode() const noexcept
    {
        return theme_.dark;
    }

    // 今のテーマ。描画・計測・ポップアップはこれを参照で持つ。ダークモードとズームの切り替えは
    // 同じ場所を書き換えるので、アドレスは変わらない。
    constexpr const Theme& GetTheme() const noexcept
    {
        return theme_;
    }

    // 今のライト/ダークのベーステーマに zoom を掛けたもの。フォントは候補の先頭のまま (起動時の先読みの推定はフォント名を使わない)。
    Theme CreateTheme(ZoomLevel zoom) const;

    // 今のテーマを作り、フォントを実在する候補に決める。フォントはテーマ間で共通なので、決めるのはここだけ。
    void Init(bool dark, ZoomLevel zoom);
    // 色だけが変わる。寸法とフォントはそのまま。戻り値は切り替えた後がダークか。
    bool ToggleDarkMode();
    // 寸法が変わる。色とフォントはそのまま。
    void ApplyZoom(ZoomLevel zoom);

    void SaveDarkMode();
    bool LoadDarkMode() const;

    void SaveZoomLevel(ZoomLevel zoom);
    ZoomLevel LoadZoomLevel() const;

private:
    static Theme MakeTheme(bool dark, ZoomLevel zoom);

    ConfigService& config_;
    // Init より前に読まれても (WM_NCHITTEST など) 不定値にならないよう値初期化する。
    Theme theme_{};
};
