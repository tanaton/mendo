#pragma once
#include <type_traits>
#include <utility>

// スコープ離脱時にクリーンアップ関数を一度だけ実行する汎用 RAII ガード。
// デストラクタのためだけの構造体をその場で定義する代わりに使う。Dismiss() で取り消せる。
template <typename F>
class ScopeGuard {
public:
    explicit ScopeGuard(F f) noexcept(std::is_nothrow_move_constructible_v<F>)
        : f_(std::move(f))
    {}
    ~ScopeGuard()
    {
        if (active_) {
            f_();
        }
    }
    ScopeGuard(ScopeGuard&& other) noexcept(std::is_nothrow_move_constructible_v<F>)
        : f_(std::move(other.f_)),
          active_(other.active_)
    {
        other.active_ = false;
    }
    ScopeGuard(const ScopeGuard&) = delete;
    ScopeGuard& operator=(const ScopeGuard&) = delete;
    ScopeGuard& operator=(ScopeGuard&&) = delete;

    void Dismiss() noexcept
    {
        active_ = false;
    }

private:
    F f_;
    bool active_ = true;
};

template <typename F>
ScopeGuard(F) -> ScopeGuard<F>;
