#pragma once
#include "document_types.h"
#include "layout_cache.h"
#include "test_helpers.h"
#include <initializer_list>
#include <memory_resource>

// RunParallel のテスト用。ノード i は Paragraph で text_top = i * 100、height = 80。
struct DirtyNodeFixture {
    std::pmr::vector<Node> nodes;
    LayoutCache cache;

    void Build(size_t n, bool all_dirty)
    {
        for (size_t i = 0; i < n; ++i) {
            nodes.push_back(MakeTextNode("x"));
        }
        cache.Resize(n);
        for (size_t i = 0; i < n; ++i) {
            cache.SetTop(i, static_cast<float>(i) * 100.0f);
            cache[i].height = 80.0f;
            cache[i].layout_dirty = all_dirty;
        }
    }

    void Build(size_t n, std::initializer_list<size_t> dirty_indices)
    {
        Build(n, false);
        for (size_t idx : dirty_indices) {
            cache[idx].layout_dirty = true;
        }
    }
};
