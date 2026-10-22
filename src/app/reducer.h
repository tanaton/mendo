#pragma once
#include "app_state.h"
#include "app_events.h"
#include "side_effect.h"

// 現在の状態を in-place で変更し、実行すべき副作用のリストを返す。
SideEffectList Reduce(AppState& state, const AppAction& action);

// 進行中の左ドラッグを終え、キャプチャを解放させる (右ボタンのジェスチャーが使っている間は持ち続ける)。
void EndLeftDrag(AppState& state, SideEffectList& effects);

// イベント→アクション変換。App が実行する。状態は呼び出し側が持って渡す。
namespace app_controller {

AppAction HandleKeyDown(const KeyDownEvent& event);
// zoom_accum は Ctrl+ホイールの delta の端数。ピンチや高分解能ホイールは WHEEL_DELTA 未満を
// 連発するため、1 ノッチ分たまるごとに 1 段だけズームする。
AppAction HandleMouseWheel(const MouseWheelEvent& event, int& zoom_accum);

} // namespace app_controller
