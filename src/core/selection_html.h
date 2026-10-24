#pragma once
#include "document_types.h"
#include <cstdint>
#include <memory_resource>
#include <optional>
#include <string>
#include <vector>

// ノード間を \r\n で連結する。区切ったテキストの続きは元のとおりにつなぐ。
std::pmr::string ExtractSelectedText(const std::pmr::vector<Node>& nodes, const TextSelection& selection);

// 戻り値は CF_HTML のフラグメント部（<!--StartFragment--> と <!--EndFragment--> の間に入る本文）。
std::pmr::string ExtractSelectedTextAsHtml(
    const std::pmr::vector<Node>& nodes,
    const TextSelection& selection,
    bool dark_mode);

struct LinkHit {
    std::string_view url; // node 内の URL を指す
    uint32_t index = 0;   // node.view_link_urls() の添字
};
[[nodiscard]] std::optional<LinkHit> FindLinkHitAtPosition(const Node& node, uint32_t text_pos);
