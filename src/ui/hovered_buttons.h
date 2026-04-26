#pragma once

// MD ペインのオーバーレイボタンのホバー状態。
// 各フィールドはホバー対象のノードインデックスで、-1 は「該当なし」を表す。
struct HoveredButtons {
    int copy = -1;
    int save = -1;
    int svg_copy = -1;

    bool operator==(const HoveredButtons&) const = default;
};
