#pragma once
#include <d2d1.h>
#include <numbers>

// 選択範囲のハイライトカラー
inline constexpr D2D1_COLOR_F SELECTION_COLOR = {0.26f, 0.56f, 0.84f, 0.3f};

// 2π（円周率の2倍）
inline constexpr float TWO_PI = std::numbers::pi_v<float> * 2.0f;

// ローディングスピナーの定数
namespace spinner {
    inline constexpr float RADIUS = 20.0f;
    inline constexpr float DOT_RADIUS = 3.0f;
    inline constexpr int DOT_COUNT = 8;
    inline constexpr float ROTATION_INCREMENT = 0.15f;
    inline constexpr float DOT_FADE_FACTOR = 0.85f;
}

// マウスホイールスクロールの倍率
inline constexpr float MOUSE_WHEEL_SCROLL_MULTIPLIER = 0.8f;

// ホバー時のヒットテスト省略判定用の距離の二乗
inline constexpr int HOVER_THROTTLE_DISTANCE_SQ = 16;

// 目次ペインの見出しレベル毎のインデント幅
inline constexpr float TOC_INDENT_PER_LEVEL = 12.0f;
