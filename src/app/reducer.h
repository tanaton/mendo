#pragma once
#include "app_state.h"
#include "app_events.h"
#include "side_effect.h"

// Reducer: アプリケーション状態遷移の唯一の実行点。
// 現在の状態を in-place で変更し、実行すべき副作用のリストを返す。
//
// C++ の制約（COM オブジェクトのコピー不可）により完全な不変性は実現できないが、
// 「Reducer が唯一の状態変更点」という規律で不変性の主要な利点を確保する。
SideEffectList Reduce(AppState& state, const AppAction& action);
