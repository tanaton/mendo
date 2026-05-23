#pragma once
#include "app_events.h"

// ステートレスなイベント→アクション変換。Shell が実行する。
namespace app_controller {

AppAction HandleKeyDown(const KeyDownEvent& event);
AppAction HandleMouseWheel(const MouseWheelEvent& event);

} // namespace app_controller
