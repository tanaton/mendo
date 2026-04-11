#pragma once
#include "ui_constants.h"
#include <d2d1.h>

// タイトルバー上のヒット領域
enum class TitleBarHitZone {
    None,        // タイトルバー外
    Caption,     // ドラッグ可能領域
    Icon,        // アプリアイコン（システムメニュー）
    OpenFile,    // ファイルを開くボタン
    Help,        // ヘルプボタン
    ThemeToggle, // ダークモード切替ボタン
    Search,      // 検索ボタン
    FileToggle,  // ファイルペイン切替ボタン
    TocToggle,   // 目次ペイン切替ボタン
    Minimize,    // 最小化ボタン
    Maximize,    // 最大化/復元ボタン
    Close,       // 閉じるボタン
};

// タイトルバーボタンの状態
struct TitleBarButton {
    D2D1_RECT_F rect{};
    bool hovered = false;
};

// カスタムタイトルバーの状態管理（Win32非依存）。
// ボタン位置計算・ヒットテスト・ホバー管理を担当。
class TitleBar {
public:
    static constexpr float BASE_HEIGHT = 32.0f;
    static constexpr float BUTTON_WIDTH = 32.0f;
    static constexpr float ICON_LEFT_MARGIN = 8.0f;
    static constexpr float ICON_SIZE = 24.0f;
    static constexpr float ICON_RIGHT_GAP = 4.0f;
    static constexpr float BUTTON_GAP = 2.0f;
    // キャプションボタン（最小化/最大化/閉じる）はタイトルバーの全高を使う
    static constexpr float CAPTION_BTN_WIDTH = 46.0f;

    constexpr float GetHeight() const noexcept { return BASE_HEIGHT; }

    // ウィンドウ幅からボタン位置を計算
    void UpdateLayout(float window_width_dip) noexcept
    {
        // ── 左側ボタン群（アイコンの右から配置）──
        // アイコン位置（タイトルバー左端、垂直中央）
        const float icon_top = (BASE_HEIGHT - ICON_SIZE) / 2.0f;
        icon_rect_ = D2D1::RectF(ICON_LEFT_MARGIN, icon_top,
            ICON_LEFT_MARGIN + ICON_SIZE, icon_top + ICON_SIZE);

        float left = ICON_LEFT_MARGIN + ICON_SIZE + ICON_RIGHT_GAP;
        open_file_.rect = D2D1::RectF(left, 0.0f, left + BUTTON_WIDTH, BASE_HEIGHT);
        left += BUTTON_WIDTH;
        search_.rect = D2D1::RectF(left, 0.0f, left + BUTTON_WIDTH, BASE_HEIGHT);
        left += BUTTON_WIDTH;
        theme_toggle_.rect = D2D1::RectF(left, 0.0f, left + BUTTON_WIDTH, BASE_HEIGHT);
        left += BUTTON_WIDTH;
        help_.rect = D2D1::RectF(left, 0.0f, left + BUTTON_WIDTH, BASE_HEIGHT);
        left += BUTTON_WIDTH;

        // ── 右側ボタン群（右端から配置）──
        float right = window_width_dip;
        close_.rect = D2D1::RectF(right - CAPTION_BTN_WIDTH, 0.0f, right, BASE_HEIGHT);
        right -= CAPTION_BTN_WIDTH;
        maximize_.rect = D2D1::RectF(right - CAPTION_BTN_WIDTH, 0.0f, right, BASE_HEIGHT);
        right -= CAPTION_BTN_WIDTH;
        minimize_.rect = D2D1::RectF(right - CAPTION_BTN_WIDTH, 0.0f, right, BASE_HEIGHT);
        right -= CAPTION_BTN_WIDTH;

        // ペイン切替ボタン（最小化ボタンの左隣から配置）
        toc_toggle_.rect = D2D1::RectF(right - BUTTON_WIDTH, 0.0f, right, BASE_HEIGHT);
        right -= BUTTON_WIDTH;
        file_toggle_.rect = D2D1::RectF(right - BUTTON_WIDTH, 0.0f, right, BASE_HEIGHT);

        // タイトルテキスト領域（左側ボタンの右からファイル切替ボタンの左まで）
        const float title_right = file_toggle_.rect.left;
        title_text_rect_ = D2D1::RectF(left, 0.0f, (title_right > left) ? title_right : left, BASE_HEIGHT);
    }

    // DIP座標でヒットテスト
    TitleBarHitZone HitTest(float dip_x, float dip_y) const noexcept
    {
        if (dip_y < 0.0f || dip_y >= BASE_HEIGHT) {
            return TitleBarHitZone::None;
        }
        if (PointInRect(dip_x, dip_y, close_.rect)) {
            return TitleBarHitZone::Close;
        }
        if (PointInRect(dip_x, dip_y, maximize_.rect)) {
            return TitleBarHitZone::Maximize;
        }
        if (PointInRect(dip_x, dip_y, minimize_.rect)) {
            return TitleBarHitZone::Minimize;
        }
        // 左側ボタン群
        if (PointInRect(dip_x, dip_y, open_file_.rect)) {
            return TitleBarHitZone::OpenFile;
        }
        if (PointInRect(dip_x, dip_y, search_.rect)) {
            return TitleBarHitZone::Search;
        }
        if (PointInRect(dip_x, dip_y, theme_toggle_.rect)) {
            return TitleBarHitZone::ThemeToggle;
        }
        if (PointInRect(dip_x, dip_y, help_.rect)) {
            return TitleBarHitZone::Help;
        }
        // 右側ボタン群
        if (PointInRect(dip_x, dip_y, file_toggle_.rect)) {
            return TitleBarHitZone::FileToggle;
        }
        if (PointInRect(dip_x, dip_y, toc_toggle_.rect)) {
            return TitleBarHitZone::TocToggle;
        }
        // アイコン領域（クリックしやすいようアイコン右のギャップまで含む）
        if (dip_x < icon_rect_.right + ICON_RIGHT_GAP) {
            return TitleBarHitZone::Icon;
        }
        return TitleBarHitZone::Caption;
    }

    // ホバー状態を設定。変化した場合trueを返す。
    bool SetHovered(TitleBarHitZone zone) noexcept
    {
        if (hovered_ == zone) {
            return false;
        }
        hovered_ = zone;
        open_file_.hovered = (zone == TitleBarHitZone::OpenFile);
        help_.hovered = (zone == TitleBarHitZone::Help);
        theme_toggle_.hovered = (zone == TitleBarHitZone::ThemeToggle);
        search_.hovered = (zone == TitleBarHitZone::Search);
        file_toggle_.hovered = (zone == TitleBarHitZone::FileToggle);
        toc_toggle_.hovered = (zone == TitleBarHitZone::TocToggle);
        minimize_.hovered = (zone == TitleBarHitZone::Minimize);
        maximize_.hovered = (zone == TitleBarHitZone::Maximize);
        close_.hovered = (zone == TitleBarHitZone::Close);
        return true;
    }

    constexpr TitleBarHitZone GetHovered() const noexcept { return hovered_; }
    constexpr const TitleBarButton& GetOpenFileButton() const noexcept { return open_file_; }
    constexpr const TitleBarButton& GetHelpButton() const noexcept { return help_; }
    constexpr const TitleBarButton& GetThemeToggleButton() const noexcept { return theme_toggle_; }
    constexpr const TitleBarButton& GetSearchButton() const noexcept { return search_; }
    constexpr const TitleBarButton& GetFileToggleButton() const noexcept { return file_toggle_; }
    constexpr const TitleBarButton& GetTocToggleButton() const noexcept { return toc_toggle_; }
    constexpr const TitleBarButton& GetMinimizeButton() const noexcept { return minimize_; }
    constexpr const TitleBarButton& GetMaximizeButton() const noexcept { return maximize_; }
    constexpr const TitleBarButton& GetCloseButton() const noexcept { return close_; }
    constexpr const D2D1_RECT_F& GetIconRect() const noexcept { return icon_rect_; }
    constexpr const D2D1_RECT_F& GetTitleTextRect() const noexcept { return title_text_rect_; }

private:
    TitleBarButton open_file_;
    TitleBarButton help_;
    TitleBarButton theme_toggle_;
    TitleBarButton search_;
    TitleBarButton file_toggle_;
    TitleBarButton toc_toggle_;
    TitleBarButton minimize_;
    TitleBarButton maximize_;
    TitleBarButton close_;
    D2D1_RECT_F icon_rect_{};
    D2D1_RECT_F title_text_rect_{};
    TitleBarHitZone hovered_ = TitleBarHitZone::None;
};
