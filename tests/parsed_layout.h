#pragma once
#include "layout.h"
#include "layout_cache.h"
#include "parser.h"
#include "test_nodes.h"
#include <deque>
#include <memory_resource>
#include <string>
#include <string_view>

// 全ノードが未計測の空キャッシュ。別のファイルを開いた直後に相当する。
inline LayoutCache MakeEmptyCache(size_t node_count)
{
    LayoutCache cache;
    cache.Resize(node_count);
    return cache;
}

// Reestimate してから範囲を計測する。範囲を省略すると全ノードを計測する。
inline void ComputeLayout(LayoutEngine& engine, const std::pmr::vector<Node>& nodes, LayoutCache& cache, float viewport_width,
                          float viewport_top = -MeasureViewportRange::kInf, float viewport_bottom = MeasureViewportRange::kInf)
{
    engine.Reestimate(nodes, cache, viewport_width);
    engine.EnsureVisibleLayout(nodes, cache, viewport_width, viewport_top, viewport_bottom);
}

struct ParsedLayout {
    std::pmr::vector<Node> nodes;
    LayoutCache cache;
};

// ノードはソース文字列を参照するため、フィクスチャの寿命まで保持する
// (呼び出し側が一時文字列を渡してもダングリングしない)。
class LayoutSourceStore {
public:
    // 範囲は ComputeLayout と同じ。省略すると全ノードを計測する。
    ParsedLayout ParseAndLayout(LayoutEngine& engine, std::string_view md, float viewport_w,
                                float viewport_top = -MeasureViewportRange::kInf, float viewport_bottom = MeasureViewportRange::kInf)
    {
        ParsedLayout r;
        r.nodes = ParseNodes(sources_.emplace_back(md));
        r.cache.Resize(r.nodes.size());
        ComputeLayout(engine, r.nodes, r.cache, viewport_w, viewport_top, viewport_bottom);
        return r;
    }

private:
    // deque は要素を移動しないため、各 string (SSO 含む) のアドレスが安定する。
    std::deque<std::string> sources_;
};
