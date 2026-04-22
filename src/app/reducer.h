#pragma once
#include "app_state.h"
#include "app_events.h"
#include "side_effect.h"

// Reducer: UI イベント由来の状態遷移ハブ。
// 現在の状態を in-place で変更し、実行すべき副作用のリストを返す。
//
// 設計方針（hybrid モデル）:
//   - UI イベント（マウス / キー / タイマ / メッセージ）の 99% (67/69 箇所) は
//     `App::Dispatch()` → `Reduce()` 経由で処理する。これが「多数派の state 遷移点」。
//   - ファイル I/O (`App::DoLoadMarkdownFile`, `App::FinishLoadMarkdownFile`) や
//     外部 URL 起動 (`App::HandleLinkClick` の ExternalUrl 分岐) のような大きな
//     命令的ワークフローは、App 層の service メソッドに集約して reducer 外で実行する。
//     これらを effect variant 化すると reducer と executor が肥大化するため、
//     pragmatic に「I/O は service、UI 遷移は reducer」の境界を選択している。
//   - `App::ShowToast` は toast 表示の 1 行 effect を直接発火する簡易経路（Dispatch
//     経由にしても単に boilerplate が増えるだけなので例外として許容）。
//
// C++ の制約（COM オブジェクトのコピー不可）により完全な不変性は実現できないが、
// 「UI イベントは必ず reducer を通る」という規律で状態追跡性を確保する。
SideEffectList Reduce(AppState& state, const AppAction& action);
