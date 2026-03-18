#pragma once
#include <deque>
#include <vector>
#include <cmath>

enum class GesturePhase { Idle, Pressed, Tracking };
enum class GestureDirection { None, Left, Right };
enum class GestureResult { None, ShowContextMenu, Back, Forward };

struct GesturePoint {
    float x = 0.0f;
    float y = 0.0f;
};

class MouseGesture {
public:
    static constexpr float GESTURE_THRESHOLD = 30.0f;
    static constexpr float MIN_POINT_DISTANCE = 2.0f;
    static constexpr int   TRAIL_MAX_POINTS = 512;

    void OnRButtonDown(float x, float y) {
        Reset();
        phase_ = GesturePhase::Pressed;
        start_x_ = x;
        start_y_ = y;
        current_x_ = x;
        current_y_ = y;
        trail_points_.clear();
        trail_points_.push_back({x, y});
    }

    void OnMouseMove(float x, float y) {
        if (phase_ == GesturePhase::Idle) return;

        current_x_ = x;
        current_y_ = y;

        float dx = x - start_x_;
        float dy = y - start_y_;
        float dist = std::sqrt(dx * dx + dy * dy);

        if (phase_ == GesturePhase::Pressed) {
            if (dist >= GESTURE_THRESHOLD) {
                phase_ = GesturePhase::Tracking;
                UpdateDirection();
            }
        }

        if (phase_ == GesturePhase::Tracking) {
            // Subsample trail points
            const auto& last = trail_points_.back();
            float pdx = x - last.x;
            float pdy = y - last.y;
            float pdist = std::sqrt(pdx * pdx + pdy * pdy);
            if (pdist >= MIN_POINT_DISTANCE) {
                if (trail_points_.size() >= static_cast<size_t>(TRAIL_MAX_POINTS)) {
                    trail_points_.pop_front();
                }
                trail_points_.push_back({x, y});
            }
            UpdateDirection();
            // Show overlay as soon as a direction is determined
            overlay_alpha_ = (direction_ != GestureDirection::None) ? 1.0f : 0.0f;
        }
    }

    GestureResult OnRButtonUp() {
        if (phase_ == GesturePhase::Idle) {
            return GestureResult::None;
        }

        if (phase_ == GesturePhase::Pressed) {
            // Small movement — context menu
            phase_ = GesturePhase::Idle;
            return GestureResult::ShowContextMenu;
        }

        // Tracking → determine result and reset to Idle
        GestureDirection dir = direction_;
        Reset();

        switch (dir) {
            case GestureDirection::Left:  return GestureResult::Back;
            case GestureDirection::Right: return GestureResult::Forward;
            default:                      return GestureResult::None;
        }
    }

    void Reset() {
        phase_ = GesturePhase::Idle;
        direction_ = GestureDirection::None;
        start_x_ = 0.0f;
        start_y_ = 0.0f;
        current_x_ = 0.0f;
        current_y_ = 0.0f;
        overlay_alpha_ = 0.0f;
        trail_points_.clear();
    }

    bool IsGestureActive() const { return phase_ == GesturePhase::Tracking; }
    bool IsOverlayVisible() const { return overlay_alpha_ > 0.0f; }
    const std::deque<GesturePoint>& GetTrailPoints() const { return trail_points_; }
    GestureDirection GetDirection() const { return direction_; }
    GesturePhase GetPhase() const { return phase_; }
    float GetOverlayAlpha() const { return overlay_alpha_; }

private:
    void UpdateDirection() {
        float dx = current_x_ - start_x_;
        float dy = current_y_ - start_y_;
        float dist = std::sqrt(dx * dx + dy * dy);

        if (dist < GESTURE_THRESHOLD || std::abs(dx) <= std::abs(dy)) {
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
    std::deque<GesturePoint> trail_points_;
};
