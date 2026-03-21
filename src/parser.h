#pragma once
#include "types.h"
#include <string>
#include <string_view>
#include <vector>
#include <memory_resource>

std::pmr::vector<Node> ParseMarkdown(const std::string& markdown_text);

// Generate GitHub-style anchor slug from heading text (exposed for testing)
std::wstring GenerateAnchorId(std::wstring_view text);
