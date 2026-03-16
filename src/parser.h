#pragma once
#include "types.h"
#include <string>
#include <vector>

std::vector<RenderNode> ParseMarkdown(const std::string& markdown_text);

// Generate GitHub-style anchor slug from heading text (exposed for testing)
std::wstring GenerateAnchorId(const std::wstring& text);
