#pragma once
#include "side_effect.h"
#include <concepts>

namespace mendo {

// ScrollDrag Started/Ended ヘルパ。SetCapture / ReleaseCapture をペアで強制発火させる。
// 対象固有処理は invocable で受け、型消去せずインライン展開する。
// 不要な callable は `mendo::no_op` を渡してスキップする。

namespace detail {

struct NoOpFn {
    static constexpr void operator()() noexcept
    {}
};

} // namespace detail

inline constexpr detail::NoOpFn no_op{};

// 「begin → SetCapture → jump → emit」の順で実行する。
//   jump: thumb-grip 外クリック時の 1st jump (Md/PaneScrollbar 用)。
//   emit: 追加 effect (InvalidateWindow 等)。
template <std::invocable Begin, std::invocable Jump, std::invocable Emit>
inline void RunScrollDragStarted(
    SideEffectList& effects,
    Begin&& begin,
    Jump&& jump,
    Emit&& emit)
{
    begin();
    PushEffect(effects, effect::SetCapture{});
    jump();
    emit();
}

// 「finalize → ReleaseCapture → emit_extra」の順で実行する。
template <std::invocable Finalize, std::invocable Emit>
inline void RunScrollDragEnded(
    SideEffectList& effects,
    Finalize&& finalize,
    Emit&& emit_extra)
{
    finalize();
    PushEffect(effects, effect::ReleaseCapture{});
    emit_extra();
}

} // namespace mendo
