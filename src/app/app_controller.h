#pragma once
#include "app_events.h"

// イベント→アクション変換。Shell が実行する。状態は呼び出し側が持って渡す。
namespace app_controller {

AppAction HandleKeyDown(const KeyDownEvent& event);
// zoom_accum は Ctrl+ホイールの delta の端数。ピンチや高分解能ホイールは WHEEL_DELTA 未満を
// 連発するため、1 ノッチ分たまるごとに 1 段だけズームする。
AppAction HandleMouseWheel(const MouseWheelEvent& event, int& zoom_accum);

} // namespace app_controller
