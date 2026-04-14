#pragma once
#include "layout_cache.h"

// 等間隔ノードの LayoutCache を構築するテスト用ヘルパー。
// 複数テストファイルで共通して使用される。
inline LayoutCache MakeUniformCache(int count, float node_height = 100.0f)
{
    LayoutCache cache;
    cache.Resize(count);
    float y = 0.0f;
    for (int i = 0; i < count; ++i) {
        cache[i].y_position = y;
        cache[i].height = node_height;
        y += node_height;
    }
    return cache;
}
