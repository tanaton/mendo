#pragma once
#include "side_effect.h"
#include <concepts>

namespace mendo {

// ScrollDrag Started/Ended ヘルパ。SetCapture / ReleaseCapture をペアで強制発火させる。
// 対象固有処理は invocable で受け、型消去せずインライン展開する。

namespace detail {

struct NoOpFn {
    static constexpr void operator()() noexcept
    {}
};

} // namespace detail

inline constexpr detail::NoOpFn no_op{};

// 「begin → SetCapture → post」の順で実行する。
//   begin: ドラッグ state 初期化 (SetCapture より前にやる必要がある)。
//   post : SetCapture 後の処理 (thumb-grip 外クリック時の 1st jump 含む)。
template <std::invocable Begin, std::invocable Post>
inline void RunScrollDragStarted(SideEffectList& effects, Begin&& begin, Post&& post)
{
    begin();
    PushEffect(effects, effect::SetCapture{});
    post();
}

template <std::invocable Begin>
inline void RunScrollDragStarted(SideEffectList& effects, Begin&& begin)
{
    RunScrollDragStarted(effects, std::forward<Begin>(begin), no_op);
}

// 「finalize → ReleaseCapture → post」の順で実行する。
template <std::invocable Finalize, std::invocable Post>
inline void RunScrollDragEnded(SideEffectList& effects, Finalize&& finalize, Post&& post)
{
    finalize();
    PushEffect(effects, effect::ReleaseCapture{});
    post();
}

template <std::invocable Finalize>
inline void RunScrollDragEnded(SideEffectList& effects, Finalize&& finalize)
{
    RunScrollDragEnded(effects, std::forward<Finalize>(finalize), no_op);
}

} // namespace mendo
