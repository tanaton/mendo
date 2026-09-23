#pragma once
#include <memory_resource>
#include <unordered_map>

// node のブロック横スクロール量。マップ未設定・未登録は 0。
inline float LookupBlockScrollX(const std::pmr::unordered_map<int, float>* scroll_x, int node_index) noexcept
{
    if (!scroll_x) {
        return 0.0f;
    }
    const auto it = scroll_x->find(node_index);
    return (it != scroll_x->end()) ? it->second : 0.0f;
}

struct BlockHScrollContext {
    const std::pmr::unordered_map<int, float>* scroll_x = nullptr;
    int hovered_block = -1;
    int drag_block = -1;

    float GetScrollX(int node_index) const noexcept
    {
        return LookupBlockScrollX(scroll_x, node_index);
    }
};
