#pragma once
#include "side_effect.h"
#include <concepts>
#include <functional>
#include <utility>

namespace mendo {

// ScrollDrag の Started / Ended 用ヘルパ。SetCapture と ReleaseCapture を
// ペアで強制発火させるための薄い骨格関数。
//
// Moved には対応関数を用意しない: 共通部分が「apply の後に emit」だけで
// 関数化する価値が薄く、呼び出し側に直書きさせる方が短くなるため。
// 早期 return 条件 (can_start / is_active) も呼び出し側の guard 句に出して、
// false 時の lambda 構築コストも避ける。
//
// 対象固有の処理は invocable (典型的には lambda) で渡し、型消去せず
// テンプレートで受けることで SBO 超過時の heap 確保と間接呼び出しを排除する。
// オプショナル callable は `mendo::no_op` を渡してスキップを表現する。

namespace detail {

struct NoOpFn {
    static constexpr void operator()() noexcept
    {}
};

} // namespace detail

inline constexpr detail::NoOpFn no_op{};

// Started: 「begin → SetCapture push → jump → emit」の順で実行する。
//   jump: thumb-grip 外クリック時の 1st jump (Md/PaneScrollbar 用)。
//         BlockHScroll や thumb 内クリックでは no_op を渡す。
//   emit: 追加 effect (InvalidateWindow 等)。不要なら no_op。
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

// Ended: 「finalize → ReleaseCapture push → emit_extra」の順で実行する。
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
