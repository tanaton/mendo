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
    // 水平ホイールイベントを処理。デルタを蓄積するのみで即時発火しない。
    // 指を離した後の Commit() 呼び出しでナビゲーションが発動する。
    // now_ms: 現在時刻（ミリ秒）。呼び出し側で GetTickCount64() 等を渡す。
    void OnHWheel(int delta, uint64_t now_ms) noexcept {
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

    // 指を離した（一定時間入力が途絶えた）タイミングで呼び出す。
    // 蓄積デルタが閾値を超えていれば Back/Forward を返し、状態をリセットする。
    SwipeResult Commit() noexcept {
        SwipeResult result = SwipeResult::None;
        if (accumulated_delta_ >= TRIGGER_THRESHOLD) {
            result = SwipeResult::Back;
        } else if (accumulated_delta_ <= -TRIGGER_THRESHOLD) {
            result = SwipeResult::Forward;
        }
        accumulated_delta_ = 0;
        last_hscroll_time_ = 0;
        return result;
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

    // 蓄積デルタが発動閾値に達していればオーバーレイを表示する。
    constexpr bool IsOverlayVisible() const noexcept {
        int abs_d = accumulated_delta_ < 0 ? -accumulated_delta_ : accumulated_delta_;
        return abs_d >= TRIGGER_THRESHOLD;
    }

    // オーバーレイの方向。 -1=戻る（右スワイプ）, 1=進む（左スワイプ）, 0=なし。
    // GestureRenderState::direction と同じ符号規約。
    constexpr int GetOverlayDirection() const noexcept {
        if (accumulated_delta_ >= TRIGGER_THRESHOLD)  return -1;  // 右スワイプ → 戻る
        if (accumulated_delta_ <= -TRIGGER_THRESHOLD) return  1;  // 左スワイプ → 進む
        return 0;
    }

    // オーバーレイ表示中は 1.0、非表示時は 0.0 を返す。
    constexpr float GetOverlayAlpha() const noexcept {
        return IsOverlayVisible() ? 1.0f : 0.0f;
    }

    // テスト・チューニング用のアクセサ
    constexpr int GetAccumulatedDelta() const noexcept { return accumulated_delta_; }

    static constexpr int TRIGGER_THRESHOLD = 400;           // ナビゲーション発動閾値（WHEEL_DELTA単位の蓄積値）
    static constexpr uint64_t AXIS_LOCK_MS = 200;           // 縦スクロール後の水平入力無視期間
    static constexpr uint64_t RESET_TIMEOUT_MS = 500;       // 蓄積リセットまでの無活動期間
    static constexpr uint64_t COMMIT_TIMEOUT_MS = 150;      // 指を離してからナビゲーション発火までの待機期間

private:
    int accumulated_delta_ = 0;
    uint64_t last_hscroll_time_ = 0;
    uint64_t last_vscroll_time_ = 0;
};
