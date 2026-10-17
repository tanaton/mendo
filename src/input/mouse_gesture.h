#pragma once
#include "ui_types.h"
#include <cmath>
#include <cstdint>
#include <deque>
#include <memory_resource>

enum class GesturePhase : uint8_t {
    Idle,
    Pressed,
    Tracking
};
enum class GestureResult : uint8_t {
    None,
    ShowContextMenu,
    Back,
    Forward
};

struct GesturePoint {
    float x = 0.0f;
    float y = 0.0f;
};

class MouseGesture {
public:
    static constexpr float GESTURE_THRESHOLD = 30.0f;
    static constexpr float GESTURE_THRESHOLD_SQ = GESTURE_THRESHOLD * GESTURE_THRESHOLD;
    static constexpr float MIN_POINT_DISTANCE = 2.0f;
    static constexpr float MIN_POINT_DISTANCE_SQ = MIN_POINT_DISTANCE * MIN_POINT_DISTANCE;
    static constexpr int TRAIL_MAX_POINTS = 512;

    void OnRButtonDown(float x, float y)
    {
        Reset();
        phase_ = GesturePhase::Pressed;
        start_x_ = x;
        start_y_ = y;
        trail_points_.emplace_back(x, y);
    }

    // 描画に表れる変化 (軌跡への点の追加・方向の変化) があれば true。
    bool OnMouseMove(float x, float y)
    {
        if (phase_ == GesturePhase::Idle) {
            return false;
        }

        const float dx = x - start_x_;
        const float dy = y - start_y_;
        const float dist_sq = dx * dx + dy * dy;

        if (phase_ == GesturePhase::Pressed && dist_sq >= GESTURE_THRESHOLD_SQ) {
            phase_ = GesturePhase::Tracking;
        }
        if (phase_ != GesturePhase::Tracking) {
            return false;
        }

        bool changed = false;
        const auto& last = trail_points_.back();
        const float pdx = x - last.x;
        const float pdy = y - last.y;
        if (pdx * pdx + pdy * pdy >= MIN_POINT_DISTANCE_SQ) {
            if (trail_points_.size() >= static_cast<size_t>(TRAIL_MAX_POINTS)) {
                trail_points_.pop_front();
            }
            trail_points_.emplace_back(x, y);
            changed = true;
        }
        const auto prev_direction = direction_;
        UpdateDirection(dx, dy, dist_sq);
        return changed || direction_ != prev_direction;
    }

    GestureResult OnRButtonUp() noexcept
    {
        if (phase_ == GesturePhase::Idle) {
            return GestureResult::None;
        }

        if (phase_ == GesturePhase::Pressed) {
            phase_ = GesturePhase::Idle;
            return GestureResult::ShowContextMenu;
        }

        const NavDirection dir = direction_;
        Reset();

        switch (dir) {
        case NavDirection::None:
            // 方向が確定しなかった右ドラッグは通常の右クリック扱いとし、
            // コンテキストメニューを表示する (イベントを握り潰してメニューを失わない)。
            return GestureResult::ShowContextMenu;
        case NavDirection::Back:
            return GestureResult::Back;
        case NavDirection::Forward:
            return GestureResult::Forward;
        }
        std::unreachable();
    }

    void Reset() noexcept
    {
        phase_ = GesturePhase::Idle;
        direction_ = NavDirection::None;
        start_x_ = 0.0f;
        start_y_ = 0.0f;
        trail_points_.clear();
    }

    constexpr bool IsGestureActive() const noexcept
    {
        return phase_ == GesturePhase::Tracking;
    }
    constexpr bool IsOverlayVisible() const noexcept
    {
        return phase_ == GesturePhase::Tracking && direction_ != NavDirection::None;
    }
    constexpr const std::pmr::deque<GesturePoint>& GetTrailPoints() const noexcept
    {
        return trail_points_;
    }
    constexpr NavDirection GetDirection() const noexcept
    {
        return direction_;
    }
    constexpr GesturePhase GetPhase() const noexcept
    {
        return phase_;
    }

private:
    void UpdateDirection(float dx, float dy, float dist_sq) noexcept
    {
        if (dist_sq < GESTURE_THRESHOLD_SQ || std::abs(dx) <= std::abs(dy)) {
            direction_ = NavDirection::None;
            return;
        }
        direction_ = (dx < 0) ? NavDirection::Back : NavDirection::Forward;
    }

    GesturePhase phase_ = GesturePhase::Idle;
    NavDirection direction_ = NavDirection::None;
    float start_x_ = 0.0f;
    float start_y_ = 0.0f;
    std::pmr::deque<GesturePoint> trail_points_;
};
