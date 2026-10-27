#pragma once
#include "app_state.h"
#include "app_events.h"
#include "side_effect.h"

// reducer が App に計測を頼む窓口。計測 (DirectWrite) は App が持つ。
class NodeLayoutAccess {
public:
    // node_index のノードを計測済みにする (検索マッチの行の位置を引けるようにする)。見ている位置は保つ。
    virtual void EnsureNodeLayout(int node_index) = 0;

protected:
    ~NodeLayoutAccess() = default;
};

// 現在の状態を in-place で変更し、実行すべき副作用のリストを返す。一覧の領域は resource から取る。
SideEffectList Reduce(AppState& state, const AppAction& action, NodeLayoutAccess& layout,
                      std::pmr::memory_resource* resource = std::pmr::get_default_resource());

SearchBarRenderState BuildSearchBarRenderState(const AppState& state);
