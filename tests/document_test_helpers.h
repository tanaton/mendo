#pragma once
// テスト専用のドキュメント関連 helper。production では別経路 (Document::FindAnchorIndex) を使うため、これらの関数は tests/ 内でのみ意味を持つ。
#include "document_types.h"
#include "document_utils.h"
#include "heading_anchor_index.h"
#include "selection_html.h"
#include <optional>
#include <memory_resource>
#include <ranges>
#include <string>
#include <string_view>
#include <vector>

inline std::pmr::string GenerateAnchorId(std::string_view text)
{
    std::pmr::string slug;
    GenerateAnchorIdInto(text, slug);
    return slug;
}

// text_pos にあるリンクの URL。
inline std::optional<std::string_view> FindLinkAtPosition(const Node& node, uint32_t text_pos)
{
    return FindLinkHitAtPosition(node, text_pos).transform([](const LinkHit& h) noexcept { return h.url; });
}

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
