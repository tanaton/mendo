#pragma once
#include "ui_constants.h"

// ローディングスピナーの回転角度と表示有効/無効だけを管理する薄いクラス。
// FileLoadService の async/preload 状態とは独立して動き、テスト時にも純粋関数的に扱える。
class LoadingAnimation {
public:
    constexpr bool IsActive() const noexcept
    {
        return active_;
    }
    constexpr float GetAngle() const noexcept
    {
        return angle_;
    }

    constexpr void Begin() noexcept
    {
        active_ = true;
        angle_ = 0.0f;
    }
    constexpr void End() noexcept
    {
        active_ = false;
    }
    constexpr void Tick() noexcept
    {
        angle_ += spinner::ROTATION_INCREMENT;
        if (angle_ > TWO_PI) {
            angle_ -= TWO_PI;
        }
    }

private:
    bool active_ = false;
    float angle_ = 0.0f;
};
