#pragma once
#include "ui_types.h"
#include <cmath>
#include <cstdint>
#include <vector>

enum class GesturePhase : uint8_t {
    Idle,
    Pressed,
    Tracking
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
                trail_points_.erase(trail_points_.begin());
            }
            trail_points_.emplace_back(x, y);
            changed = true;
        }
        const auto prev_direction = direction_;
        UpdateDirection(dx, dy, dist_sq);
        return changed || direction_ != prev_direction;
    }

    // 確定した方向を返して Idle に戻る。動かさずに離したときと、方向が確定しなかった右ドラッグは None。
    NavDirection OnRButtonUp() noexcept
    {
        const NavDirection dir = direction_;
        Reset();
        return dir;
    }

    void Reset() noexcept
    {
        phase_ = GesturePhase::Idle;
        direction_ = NavDirection::None;
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
    constexpr const std::vector<DipPoint>& GetTrailPoints() const noexcept
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
    // clear で容量を残し、ジェスチャーごとの確保を避ける。
    std::vector<DipPoint> trail_points_;
};

// タッチパッド水平スクロール（WM_MOUSEHWHEEL）を蓄積し、
// 閾値を超えたら戻る/進むナビゲーションを発火するジェスチャー検出器。
//
// 軸ロック: 直近の縦スクロールから一定時間内は水平入力を無視する。
// タイムアウト: 一定時間水平入力がなければ蓄積をリセットする。
class SwipeDetector {
public:
    constexpr void OnHWheel(int delta, uint64_t now_ms) noexcept
    {
        if (now_ms - last_vscroll_time_ < AXIS_LOCK_MS) {
            return;
        }

        if (last_hscroll_time_ != 0 && now_ms - last_hscroll_time_ > RESET_TIMEOUT_MS) {
            accumulated_delta_ = 0;
        }

        last_hscroll_time_ = now_ms;
        accumulated_delta_ += delta;
    }

    constexpr NavDirection Commit() noexcept
    {
        const NavDirection result = GetOverlayDirection();
        accumulated_delta_ = 0;
        last_hscroll_time_ = 0;
        return result;
    }

    constexpr void NotifyVScroll(uint64_t now_ms) noexcept
    {
        last_vscroll_time_ = now_ms;
        accumulated_delta_ = 0;
    }

    constexpr bool IsOverlayVisible() const noexcept
    {
        return GetOverlayDirection() != NavDirection::None;
    }

    // 右スワイプで戻る、左スワイプで進む。
    constexpr NavDirection GetOverlayDirection() const noexcept
    {
        if (accumulated_delta_ >= TRIGGER_THRESHOLD) {
            return NavDirection::Back;
        }
        if (accumulated_delta_ <= -TRIGGER_THRESHOLD) {
            return NavDirection::Forward;
        }
        return NavDirection::None;
    }

    static constexpr int TRIGGER_THRESHOLD = 400;      // ナビゲーション発動閾値（WHEEL_DELTA単位の蓄積値）
    static constexpr uint64_t AXIS_LOCK_MS = 200;      // 縦スクロール後の水平入力無視期間
    static constexpr uint64_t RESET_TIMEOUT_MS = 500;  // 蓄積リセットまでの無活動期間
    static constexpr uint64_t COMMIT_TIMEOUT_MS = 150; // 指を離してからナビゲーション発火までの待機期間

private:
    int accumulated_delta_ = 0;
    uint64_t last_hscroll_time_ = 0;
    uint64_t last_vscroll_time_ = 0;
};
