#pragma once
#include <memory_resource>
#include <string>
#include <string_view>
#include <algorithm>

// トースト通知の状態管理。
// App側でタイマー駆動の Tick() 呼び出しによりフェードアウトを進行させる。
class ToastNotifier {
public:
    static constexpr float INITIAL_ALPHA = 2.5f; // 約0.8秒ホールド + 約0.5秒フェードアウト（合計~1.3秒）
    static constexpr float FADE_SPEED = 0.03f;   // 16ms/tickで約83tick

    constexpr void Show(std::wstring_view message)
    {
        message_ = message;
        alpha_ = INITIAL_ALPHA;
    }

    // タイマーティックごとに呼び出す。まだ表示中なら true を返す。
    constexpr bool Tick() noexcept
    {
        if (alpha_ <= 0.0f) {
            return false;
        }
        alpha_ -= FADE_SPEED;
        if (alpha_ <= 0.0f) {
            alpha_ = 0.0f;
            message_.clear();
            return false;
        }
        return true;
    }

    constexpr void Reset() noexcept
    {
        alpha_ = 0.0f;
        message_.clear();
    }

    constexpr bool IsVisible() const noexcept
    {
        return alpha_ > 0.0f;
    }
    constexpr float GetRenderAlpha() const noexcept
    {
        return std::min(alpha_, 1.0f);
    }
    constexpr float GetAlpha() const noexcept
    {
        return alpha_;
    }
    constexpr std::wstring_view GetMessage() const noexcept
    {
        return message_;
    }

private:
    std::pmr::wstring message_;
    float alpha_ = 0.0f;
};
