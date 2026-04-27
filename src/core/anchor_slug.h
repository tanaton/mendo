#pragma once
#include <memory_resource>
#include <string>
#include <string_view>

// 見出しテキストからGitHubスタイルのアンカースラグを生成する。
// ASCII は小文字化、空白は '-'、CJK 文字は保持しつつ句読点・記号はスキップする。
[[nodiscard]] std::pmr::wstring GenerateAnchorId(std::wstring_view text);
