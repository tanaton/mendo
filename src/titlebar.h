#pragma once
#include "ui_constants.h"
#include <d2d1.h>

// タイトルバー上のヒット領域
enum class TitleBarHitZone {
    None,        // タイトルバー外
    Caption,     // ドラッグ可能領域
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
    static constexpr float BUTTON_LEFT_MARGIN = 8.0f;
    static constexpr float BUTTON_GAP = 2.0f;
    // キャプションボタン（最小化/最大化/閉じる）はタイトルバーの全高を使う
    static constexpr float CAPTION_BTN_WIDTH = 46.0f;

    constexpr float GetHeight() const noexcept { return BASE_HEIGHT; }

    // ウィンドウ幅からボタン位置を計算
    void UpdateLayout(float window_width_dip) noexcept {
        // キャプションボタン（右端から配置、全高を使用）
        float right = window_width_dip;
        close_.rect = D2D1::RectF(right - CAPTION_BTN_WIDTH, 0.0f, right, BASE_HEIGHT);
        right -= CAPTION_BTN_WIDTH;
        maximize_.rect = D2D1::RectF(right - CAPTION_BTN_WIDTH, 0.0f, right, BASE_HEIGHT);
        right -= CAPTION_BTN_WIDTH;
        minimize_.rect = D2D1::RectF(right - CAPTION_BTN_WIDTH, 0.0f, right, BASE_HEIGHT);
        right -= CAPTION_BTN_WIDTH;

        // ペイン切替ボタン（キャプションボタンの左隣に配置）
        toc_toggle_.rect = D2D1::RectF(right - BUTTON_WIDTH, 0.0f, right, BASE_HEIGHT);
        right -= BUTTON_WIDTH;
        file_toggle_.rect = D2D1::RectF(right - BUTTON_WIDTH, 0.0f, right, BASE_HEIGHT);

        // タイトルテキスト領域（左端からペインボタンの左まで）
        float title_left = BUTTON_LEFT_MARGIN;
        float title_right = file_toggle_.rect.left;
        title_text_rect_ = D2D1::RectF(title_left, 0.0f, (title_right > title_left) ? title_right : title_left, BASE_HEIGHT);
    }

    // DIP座標でヒットテスト
    TitleBarHitZone HitTest(float dip_x, float dip_y) const noexcept {
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
        if (PointInRect(dip_x, dip_y, file_toggle_.rect)) {
            return TitleBarHitZone::FileToggle;
        }
        if (PointInRect(dip_x, dip_y, toc_toggle_.rect)) {
            return TitleBarHitZone::TocToggle;
        }
        return TitleBarHitZone::Caption;
    }

    // ホバー状態を設定。変化した場合trueを返す。
    bool SetHovered(TitleBarHitZone zone) noexcept {
        if (hovered_ == zone) {
            return false;
        }
        hovered_ = zone;
        file_toggle_.hovered = (zone == TitleBarHitZone::FileToggle);
        toc_toggle_.hovered = (zone == TitleBarHitZone::TocToggle);
        minimize_.hovered = (zone == TitleBarHitZone::Minimize);
        maximize_.hovered = (zone == TitleBarHitZone::Maximize);
        close_.hovered = (zone == TitleBarHitZone::Close);
        return true;
    }

    constexpr TitleBarHitZone GetHovered() const noexcept { return hovered_; }
    constexpr const TitleBarButton& GetFileToggleButton() const noexcept { return file_toggle_; }
    constexpr const TitleBarButton& GetTocToggleButton() const noexcept { return toc_toggle_; }
    constexpr const TitleBarButton& GetMinimizeButton() const noexcept { return minimize_; }
    constexpr const TitleBarButton& GetMaximizeButton() const noexcept { return maximize_; }
    constexpr const TitleBarButton& GetCloseButton() const noexcept { return close_; }
    constexpr const D2D1_RECT_F& GetTitleTextRect() const noexcept { return title_text_rect_; }

private:
    TitleBarButton file_toggle_;
    TitleBarButton toc_toggle_;
    TitleBarButton minimize_;
    TitleBarButton maximize_;
    TitleBarButton close_;
    D2D1_RECT_F title_text_rect_{};
    TitleBarHitZone hovered_ = TitleBarHitZone::None;
};
