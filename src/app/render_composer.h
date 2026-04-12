#pragma once
#include "app_state.h"
#include "renderer.h"

// AppState → 描画パラメータの変換を担当する。
// App から描画ステート構築の責務を分離し、状態と描画の明確な境界を作る。
namespace render_composer {

GestureRenderState BuildGestureState(const AppState& state);

SidePaneState BuildSidePaneState(const AppState& state, const PaneLayout& layout);

TitleBarRenderState BuildTitleBarState(const AppState& state, float window_width, bool is_dark_mode, bool is_maximized);

ToastRenderState BuildToastState(const AppState& state);

SearchBarRenderState BuildSearchBarState(const AppState& state);

} // namespace render_composer
