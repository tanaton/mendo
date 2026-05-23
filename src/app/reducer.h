#pragma once
#include "app_state.h"
#include "app_events.h"
#include "side_effect.h"

// 現在の状態を in-place で変更し、実行すべき副作用のリストを返す。
SideEffectList Reduce(AppState& state, const AppAction& action);
