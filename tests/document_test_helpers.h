#pragma once
// テスト専用のドキュメント関連 helper。production では別経路 (Document::FindAnchorIndex,
// Document::BuildHeadingIndices) を使うため、これらの関数は tests/ 内でのみ意味を持つ。
#include "doc_text.h"
#include "document_types.h"
#include "document_utils.h"
#include "toc.h"
#include <memory_resource>
#include <ranges>
#include <string_view>

// 指定 NodeType の最初のインデックスを返す。テストで「パース結果から特定種別の
// ノードを 1 つ取り出す」用途。production には対応する索引 (image_node_indices_ 等)
// があるため本関数は使わない。
inline int FindFirstNodeIndexByType(const std::pmr::vector<Node>& nodes, NodeType type) noexcept
{
    for (size_t i = 0; i < nodes.size(); ++i) {
        if (nodes[i].type == type) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

// 全ノードを線形走査して、見出しの anchor_id が `anchor` (大文字小文字無視) と
// 一致する最初のインデックスを返す。production の Document::FindAnchorIndex は
// 事前構築した anchor_index_ ハッシュ経由で同じ結果を返すため本関数は使わない。
// パーサ後の Node 配列を直接使うテストの便宜のために残す。
inline int FindAnchorNodeIndexLinear(const std::pmr::vector<Node>& nodes, std::string_view anchor)
{
    if (anchor.empty()) {
        return -1;
    }
    const std::pmr::string target = ToLowerAsciiCopy(anchor);
    for (const auto& [i, node] : nodes | std::views::enumerate) {
        if (node.type == NodeType::Heading && node.anchor_id() == target) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

// 全ノードを走査して TableOfContents を構築する。production の
// Document::BuildHeadingIndices は事前に集めた heading_indices 配列を使うため
// 本関数は使わない。Node から直接 toc を組むテストのために残す。
inline void BuildTocFromNodes(TableOfContents& toc, const std::pmr::vector<Node>& nodes)
{
    toc.Clear();
    for (const auto& [i, node] : nodes | std::views::enumerate) {
        if (node.type == NodeType::Heading) {
            toc.AddEntry(node, static_cast<int>(i));
        }
    }
}
