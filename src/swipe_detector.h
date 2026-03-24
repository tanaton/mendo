#pragma once
#include <cstdint>

enum class SwipeResult { None, Back, Forward };

// タッチパッド水平スクロール（WM_MOUSEHWHEEL）を蓄積し、
// 閾値を超えたら戻る/進むナビゲーションを発火するジェスチャー検出器。
//
// 軸ロック: 直近の縦スクロールから一定時間内は水平入力を無視する。
// タイムアウト: 一定時間水平入力がなければ蓄積をリセットする。
class SwipeDetector {
public:
    // 水平ホイールイベントを処理。
    // ナビゲーションが発動した場合 Back/Forward を返す。
    // now_ms: 現在時刻（ミリ秒）。呼び出し側で GetTickCount64() 等を渡す。
    SwipeResult OnHWheel(int delta, uint64_t now_ms) noexcept {
        // 直近の縦スクロールから一定時間内なら無視（軸ロック）
        if (now_ms - last_vscroll_time_ < AXIS_LOCK_MS) {
            return SwipeResult::None;
        }

        // 前回の水平イベントから一定時間経過していたらリセット
        if (last_hscroll_time_ != 0 && now_ms - last_hscroll_time_ > RESET_TIMEOUT_MS) {
            accumulated_delta_ = 0;
        }

        last_hscroll_time_ = now_ms;
        accumulated_delta_ += delta;

        // 右方向スワイプ（正のdelta）→ 戻る（ブラウザと同じ慣習）
        if (accumulated_delta_ >= TRIGGER_THRESHOLD) {
            accumulated_delta_ = 0;
            return SwipeResult::Back;
        }
        // 左方向スワイプ（負のdelta）→ 進む
        if (accumulated_delta_ <= -TRIGGER_THRESHOLD) {
            accumulated_delta_ = 0;
            return SwipeResult::Forward;
        }

        return SwipeResult::None;
    }

    // 縦スクロールイベントが発生したことを通知する。
    // 軸ロックの基準時刻を更新し、蓄積中のデルタをリセットする。
    void NotifyVScroll(uint64_t now_ms) noexcept {
        last_vscroll_time_ = now_ms;
        accumulated_delta_ = 0;
    }

    void Reset() noexcept {
        accumulated_delta_ = 0;
        last_hscroll_time_ = 0;
        last_vscroll_time_ = 0;
    }

    // ---- オーバーレイ表示用 ----

    // 蓄積デルタが最小値を超えていればオーバーレイを表示する。
    constexpr bool IsOverlayVisible() const noexcept {
        int abs_d = accumulated_delta_ < 0 ? -accumulated_delta_ : accumulated_delta_;
        return abs_d >= OVERLAY_MIN_DELTA;
    }

    // オーバーレイの方向。 -1=戻る（右スワイプ）, 1=進む（左スワイプ）, 0=なし。
    // GestureRenderState::direction と同じ符号規約。
    constexpr int GetOverlayDirection() const noexcept {
        if (accumulated_delta_ >= OVERLAY_MIN_DELTA)  return -1;  // 右スワイプ → 戻る
        if (accumulated_delta_ <= -OVERLAY_MIN_DELTA) return  1;  // 左スワイプ → 進む
        return 0;
    }

    // 蓄積の進捗を 0.0〜1.0 で返す。
    constexpr float GetOverlayAlpha() const noexcept {
        int abs_d = accumulated_delta_ < 0 ? -accumulated_delta_ : accumulated_delta_;
        if (abs_d < OVERLAY_MIN_DELTA) return 0.0f;
        float raw = static_cast<float>(abs_d) / static_cast<float>(TRIGGER_THRESHOLD);
        return raw < 1.0f ? raw : 1.0f;
    }

    // テスト・チューニング用のアクセサ
    constexpr int GetAccumulatedDelta() const noexcept { return accumulated_delta_; }

    static constexpr int TRIGGER_THRESHOLD = 400;           // ナビゲーション発動閾値（WHEEL_DELTA単位の蓄積値）
    static constexpr int OVERLAY_MIN_DELTA = 40;            // オーバーレイ表示開始の最小蓄積値
    static constexpr uint64_t AXIS_LOCK_MS = 200;           // 縦スクロール後の水平入力無視期間
    static constexpr uint64_t RESET_TIMEOUT_MS = 500;       // 蓄積リセットまでの無活動期間

private:
    int accumulated_delta_ = 0;
    uint64_t last_hscroll_time_ = 0;
    uint64_t last_vscroll_time_ = 0;
};
