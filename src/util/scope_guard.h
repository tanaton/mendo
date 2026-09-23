#pragma once
#include <type_traits>
#include <utility>

// スコープ離脱時にクリーンアップ関数を一度だけ実行する汎用 RAII ガード。
// デストラクタのためだけの構造体をその場で定義する代わりに使う。
// prvalue 初期化 (保証付きコピー省略) 専用のためコピー/ムーブは持たない。
template <typename F>
class ScopeGuard {
public:
    explicit ScopeGuard(F f) noexcept(std::is_nothrow_move_constructible_v<F>)
        : f_(std::move(f))
    {}
    ~ScopeGuard()
    {
        f_();
    }
    ScopeGuard(const ScopeGuard&) = delete;
    ScopeGuard& operator=(const ScopeGuard&) = delete;

private:
    F f_;
};

template <typename F>
ScopeGuard(F) -> ScopeGuard<F>;
