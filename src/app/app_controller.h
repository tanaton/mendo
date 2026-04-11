#pragma once
#include "app_events.h"

// ステートレスなイベント→アクション変換器。
// ユーザー入力イベントを高レベルのアプリアクションに変換し、
// Shell (MainWindow) が実行する。
class AppController {
public:
    AppAction HandleKeyDown(const KeyDownEvent& event) const;
    AppAction HandleMouseWheel(const MouseWheelEvent& event) const;
};
