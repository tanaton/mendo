#pragma once
#include <deque>
#include <vector>
#include <cmath>
#include <memory_resource>

enum class GesturePhase : uint8_t {
    Idle,
    Pressed,
    Tracking
};
enum class GestureDirection : uint8_t {
    None,
    Left,
    Right
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
        current_x_ = x;
        current_y_ = y;
        trail_points_.clear();
        trail_points_.emplace_back(x, y);
    }

    void OnMouseMove(float x, float y)
    {
        if (phase_ == GesturePhase::Idle) {
            return;
        }

        current_x_ = x;
        current_y_ = y;

        const float dx = x - start_x_;
        const float dy = y - start_y_;
        const float dist_sq = dx * dx + dy * dy;

        if (phase_ == GesturePhase::Pressed) {
            if (dist_sq >= GESTURE_THRESHOLD_SQ) {
                phase_ = GesturePhase::Tracking;
                UpdateDirection();
            }
        }

        if (phase_ == GesturePhase::Tracking) {
            // 軌跡ポイントをサブサンプリング
            const auto& last = trail_points_.back();
            const float pdx = x - last.x;
            const float pdy = y - last.y;
            if (pdx * pdx + pdy * pdy >= MIN_POINT_DISTANCE_SQ) {
                if (trail_points_.size() >= static_cast<size_t>(TRAIL_MAX_POINTS)) {
                    trail_points_.pop_front();
                }
                trail_points_.emplace_back(x, y);
            }
            UpdateDirection();
            // 方向が決定されたらすぐにオーバーレイを表示する
            overlay_alpha_ = (direction_ != GestureDirection::None) ? 1.0f : 0.0f;
        }
    }

    GestureResult OnRButtonUp() noexcept
    {
        if (phase_ == GesturePhase::Idle) {
            return GestureResult::None;
        }

        if (phase_ == GesturePhase::Pressed) {
            // 小さな移動 — コンテキストメニュー
            phase_ = GesturePhase::Idle;
            return GestureResult::ShowContextMenu;
        }

        // トラッキング中 → 結果を判定してIdleにリセット
        const GestureDirection dir = direction_;
        Reset();

        switch (dir) {
        case GestureDirection::Left:
            return GestureResult::Back;
        case GestureDirection::Right:
            return GestureResult::Forward;
        default:
            return GestureResult::None;
        }
    }

    void Reset() noexcept
    {
        phase_ = GesturePhase::Idle;
        direction_ = GestureDirection::None;
        start_x_ = 0.0f;
        start_y_ = 0.0f;
        current_x_ = 0.0f;
        current_y_ = 0.0f;
        overlay_alpha_ = 0.0f;
        trail_points_.clear();
    }

    constexpr bool IsGestureActive() const noexcept
    {
        return phase_ == GesturePhase::Tracking;
    }
    constexpr bool IsOverlayVisible() const noexcept
    {
        return overlay_alpha_ > 0.0f;
    }
    constexpr const std::pmr::deque<GesturePoint>& GetTrailPoints() const noexcept
    {
        return trail_points_;
    }
    constexpr GestureDirection GetDirection() const noexcept
    {
        return direction_;
    }
    constexpr GesturePhase GetPhase() const noexcept
    {
        return phase_;
    }
    constexpr float GetOverlayAlpha() const noexcept
    {
        return overlay_alpha_;
    }

private:
    void UpdateDirection() noexcept
    {
        const float dx = current_x_ - start_x_;
        const float dy = current_y_ - start_y_;
        const float dist_sq = dx * dx + dy * dy;

        if (dist_sq < GESTURE_THRESHOLD_SQ || std::abs(dx) <= std::abs(dy)) {
            direction_ = GestureDirection::None;
            return;
        }
        direction_ = (dx < 0) ? GestureDirection::Left : GestureDirection::Right;
    }

    GesturePhase phase_ = GesturePhase::Idle;
    GestureDirection direction_ = GestureDirection::None;
    float start_x_ = 0.0f;
    float start_y_ = 0.0f;
    float current_x_ = 0.0f;
    float current_y_ = 0.0f;
    float overlay_alpha_ = 0.0f;
    std::pmr::deque<GesturePoint> trail_points_;
};
