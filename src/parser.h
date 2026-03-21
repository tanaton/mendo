#pragma once
#include "types.h"
#include <string>
#include <string_view>
#include <vector>
#include <memory_resource>

[[nodiscard]] std::pmr::vector<Node> ParseMarkdown(std::string_view markdown_text);

// 見出しテキストからGitHubスタイルのアンカースラグを生成する（テスト用に公開）
[[nodiscard]] std::pmr::wstring GenerateAnchorId(std::wstring_view text);
