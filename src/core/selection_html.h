#pragma once
#include "document_types.h"
#include <cstdint>
#include <memory_resource>
#include <optional>
#include <string>
#include <vector>

// ノード間を \r\n で連結。
std::pmr::string ExtractSelectedText(const std::pmr::vector<Node>& nodes, const TextSelection& selection);

// 戻り値は CF_HTML のフラグメント部（<!--StartFragment--> と <!--EndFragment--> の間に入る本文）。
std::pmr::string ExtractSelectedTextAsHtml(
    const std::pmr::vector<Node>& nodes,
    const TextSelection& selection,
    bool dark_mode = false);

// node が CodeBlock 以外の場合は空文字列を返す。
std::pmr::string BuildCodeBlockHtmlFragment(const Node& node, bool dark_mode);

[[nodiscard]] std::optional<std::pmr::string> FindLinkAtPosition(const Node& node, uint32_t text_pos);
