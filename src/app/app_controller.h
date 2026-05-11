#pragma once
#include "app_events.h"

// ステートレスなイベント→アクション変換器。
// ユーザー入力イベントを高レベルのアプリアクションに変換し、
// Shell (MainWindow) が実行する。
namespace app_controller {

AppAction HandleKeyDown(const KeyDownEvent& event);
AppAction HandleMouseWheel(const MouseWheelEvent& event);

} // namespace app_controller
