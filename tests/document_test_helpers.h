#pragma once
// テスト専用のドキュメント関連 helper。production では別経路 (Document::FindAnchorIndex,
// Document::BuildHeadingIndices) を使うため、これらの関数は tests/ 内でのみ意味を持つ。
#include "document_types.h"
#include "document_utils.h"
#include "heading_anchor_index.h"
#include "toc.h"
#include <memory_resource>
#include <ranges>
#include <string>
#include <string_view>
#include <vector>

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

// "Paragraph 0<suffix>\n\nParagraph 1<suffix>\n\n..." の n 段落 Markdown を返す。
inline std::string MakeParagraphs(int n, std::string_view suffix = {})
{
    std::string md;
    for (int i = 0; i < n; ++i) {
        md += "Paragraph ";
        md += std::to_string(i);
        md += suffix;
        md += "\n\n";
    }
    return md;
}

// 各ノードに Document::FindAnchorIndex と同じ規則で付くアンカー。見出し以外は空。
inline std::vector<std::string> HeadingAnchors(const std::pmr::vector<Node>& nodes)
{
    HeadingAnchorIndex index;
    std::vector<std::string> anchors(nodes.size());
    for (const auto& [i, node] : nodes | std::views::enumerate) {
        if (node.type == NodeType::Heading) {
            anchors[i] = index.Add(node.GetText(), static_cast<int>(i));
        }
    }
    return anchors;
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
