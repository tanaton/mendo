#pragma once
#include "document_types.h"
#include <string>
#include <vector>

// ノード間を \r\n で連結する。区切ったテキストの続きは元のとおりにつなぐ。
std::string ExtractSelectedText(std::span<const Node> nodes, const TextSelection& selection);

// 戻り値は CF_HTML のフラグメント部（<!--StartFragment--> と <!--EndFragment--> の間に入る本文）。
std::string ExtractSelectedTextAsHtml(
    std::span<const Node> nodes,
    const TextSelection& selection,
    bool dark_mode);
