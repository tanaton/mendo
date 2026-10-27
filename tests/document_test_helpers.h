#pragma once
// テスト専用のドキュメント関連 helper。production では別経路 (Document::FindAnchorIndex) を使うため、これらの関数は tests/ 内でのみ意味を持つ。
#include "anchor.h"
#include "document_types.h"
#include "document_utils.h"
#include "selection_html.h"
#include <algorithm>
#include <format>
#include <iterator>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <vector>

// Alert の先頭に置くアイコンとラベル。GitHub と同じ表記を、パーサの表とは別に書いて照合する。
inline std::string_view AlertHeadText(AlertType type)
{
    switch (type) {
    case AlertType::Note:
        return "ℹ Note";
    case AlertType::Tip:
        return "\xF0\x9F\x92\xA1 Tip";
    case AlertType::Important:
        return "❗ Important";
    case AlertType::Warning:
        return "⚠ Warning";
    case AlertType::Caution:
        return "⛔ Caution";
    default:
        return "";
    }
}

inline std::string GenerateAnchorId(std::string_view text)
{
    std::string slug;
    GenerateAnchorIdInto(text, slug);
    return slug;
}

// text_pos にあるリンクの URL。
inline std::optional<std::string_view> FindLinkAtPosition(const Node& node, uint32_t text_pos)
{
    return FindLinkHitAtPosition(node, text_pos).transform([](const LinkHit& h) static noexcept { return h.url; });
}

// 指定 NodeType の最初のインデックスを返す。テストで「パース結果から特定種別の
// ノードを 1 つ取り出す」用途。production には対応する索引 (image_node_indices_ 等)
// があるため本関数は使わない。
inline int FindFirstNodeIndexByType(const std::vector<Node>& nodes, NodeType type) noexcept
{
    const auto it = std::ranges::find(nodes, type, &Node::type);
    return it != nodes.end() ? static_cast<int>(it - nodes.begin()) : -1;
}

// "line 0 value=0\nline 1 value=7\n..." の count 行。各行の先頭に prefix を付ける。
inline std::string MakeNumberedLines(size_t count, std::string_view prefix = {})
{
    std::string out;
    for (size_t i = 0; i < count; ++i) {
        std::format_to(std::back_inserter(out), "{}line {} value={}\n", prefix, i, i * 7);
    }
    return out;
}

// "line 0\nline 1\n..." の n 行を持つコードブロックの Markdown を返す。
inline std::string MakeCodeBlockMd(int n)
{
    std::string md = "```\n";
    for (int i = 0; i < n; ++i) {
        md += "line ";
        md += std::to_string(i);
        md += "\n";
    }
    md += "```\n";
    return md;
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
inline std::vector<std::string> HeadingAnchors(const std::vector<Node>& nodes)
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
