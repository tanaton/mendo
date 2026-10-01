#pragma once
#include "layout.h"
#include "layout_cache.h"
#include "parser.h"
#include <deque>
#include <memory_resource>
#include <string>
#include <string_view>

struct ParsedLayout {
    std::pmr::vector<Node> nodes;
    LayoutCache cache;
};

// ノードはソース文字列を参照するため、フィクスチャの寿命まで保持する
// (呼び出し側が一時文字列を渡してもダングリングしない)。
class LayoutSourceStore {
public:
    ParsedLayout ParseAndLayout(LayoutEngine& engine, std::string_view md, float viewport_w)
    {
        ParsedLayout r;
        r.nodes = ParseMarkdown(sources_.emplace_back(md)).nodes;
        r.cache.Resize(r.nodes.size());
        engine.ComputeLayout(r.nodes, r.cache, viewport_w);
        return r;
    }

private:
    // deque は要素を移動しないため、各 string (SSO 含む) のアドレスが安定する。
    std::deque<std::string> sources_;
};
