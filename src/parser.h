#pragma once
#include "types.h"
#include <string>
#include <string_view>
#include <vector>
#include <memory_resource>

std::pmr::vector<Node> ParseMarkdown(const std::string& markdown_text);

// 見出しテキストからGitHubスタイルのアンカースラグを生成する（テスト用に公開）
std::wstring GenerateAnchorId(std::wstring_view text);
