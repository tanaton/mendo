#pragma once
#include <cstdint>

enum class SwipeResult : uint8_t {
    None,
    Back,
    Forward
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
        // 直近の縦スクロールから一定時間内なら無視（軸ロック）
        if (now_ms - last_vscroll_time_ < AXIS_LOCK_MS) {
            return;
        }

        // 前回の水平イベントから一定時間経過していたらリセット
        if (last_hscroll_time_ != 0 && now_ms - last_hscroll_time_ > RESET_TIMEOUT_MS) {
            accumulated_delta_ = 0;
        }

        last_hscroll_time_ = now_ms;
        accumulated_delta_ += delta;
    }

    constexpr SwipeResult Commit() noexcept
    {
        SwipeResult result = SwipeResult::None;
        if (accumulated_delta_ >= TRIGGER_THRESHOLD) {
            result = SwipeResult::Back;
        }
        else if (accumulated_delta_ <= -TRIGGER_THRESHOLD) {
            result = SwipeResult::Forward;
        }
        accumulated_delta_ = 0;
        last_hscroll_time_ = 0;
        return result;
    }

    constexpr void NotifyVScroll(uint64_t now_ms) noexcept
    {
        last_vscroll_time_ = now_ms;
        accumulated_delta_ = 0;
    }

    constexpr void Reset() noexcept
    {
        accumulated_delta_ = 0;
        last_hscroll_time_ = 0;
        last_vscroll_time_ = 0;
    }

    // ---- オーバーレイ表示用 ----

    constexpr bool IsOverlayVisible() const noexcept
    {
        const int abs_d = accumulated_delta_ < 0 ? -accumulated_delta_ : accumulated_delta_;
        return abs_d >= TRIGGER_THRESHOLD;
    }

    constexpr int GetOverlayDirection() const noexcept
    {
        if (accumulated_delta_ >= TRIGGER_THRESHOLD) {
            return -1; // 右スワイプ → 戻る
        }
        if (accumulated_delta_ <= -TRIGGER_THRESHOLD) {
            return 1; // 左スワイプ → 進む
        }
        return 0;
    }

    constexpr float GetOverlayAlpha() const noexcept
    {
        return IsOverlayVisible() ? 1.0f : 0.0f;
    }

    constexpr int GetAccumulatedDelta() const noexcept
    {
        return accumulated_delta_;
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
