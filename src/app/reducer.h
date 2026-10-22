#pragma once
#include "app_state.h"
#include "app_events.h"
#include "side_effect.h"

// 現在の状態を in-place で変更し、実行すべき副作用のリストを返す。
SideEffectList Reduce(AppState& state, const AppAction& action);

// 進行中の左ドラッグを終え、キャプチャを解放させる (右ボタンのジェスチャーが使っている間は持ち続ける)。
void EndLeftDrag(AppState& state, SideEffectList& effects);
