#pragma once
#include "document_types.h"
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

// ノード間を \r\n で連結する。区切ったテキストの続きは元のとおりにつなぐ。
std::string ExtractSelectedText(const std::vector<Node>& nodes, const TextSelection& selection);

// 戻り値は CF_HTML のフラグメント部（<!--StartFragment--> と <!--EndFragment--> の間に入る本文）。
std::string ExtractSelectedTextAsHtml(
    const std::vector<Node>& nodes,
    const TextSelection& selection,
    bool dark_mode);

struct LinkHit {
    std::string_view url; // node 内の URL を指す
    uint32_t index = 0;   // node.view_link_urls() の添字
};
[[nodiscard]] std::optional<LinkHit> FindLinkHitAtPosition(const Node& node, uint32_t text_pos);

// ツールチップに出す画像の alt と src。
struct ImageHit {
    std::string_view src;
    // 空なら alt の無い画像。
    std::string_view alt;
    // 画像の範囲の node のテキストでの位置。同じノードの画像を見分ける。
    uint32_t start = 0;
};
[[nodiscard]] std::optional<ImageHit> FindImageHitAtPosition(const Node& node, uint32_t text_pos);
