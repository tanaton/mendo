#pragma once
#include "document_types.h"
#include "reload.h"
#include "selection_html.h"
#include <cstdint>
#include <memory_resource>
#include <string>
#include <string_view>

struct WordBoundary {
    uint32_t start = 0;
    uint32_t end = 0;
    bool found = false;
};

// カテゴリ: ASCII単語(英数+_) / ひらがな / カタカナ / 漢字 / 全角英数 / その他。
// 「その他」(空白・記号など) は {0, 0, false}。形態素解析は行わない (UAX#29 簡易版)。
// pos がマルチバイト/サロゲートの途中を指した場合は code point 先頭へスナップする。
WordBoundary FindWordBoundaries(std::string_view text, uint32_t pos) noexcept;
WordBoundary FindWordBoundaries(std::wstring_view text, uint32_t pos) noexcept;

std::pmr::string ToLowerAscii(std::string_view text);

// GitHub スタイル: ASCII 小文字化、空白→'-'、CJK 保持、句読点スキップ。
std::pmr::string GenerateAnchorId(std::string_view text);

// 呼び出し側で allocator を揃えたい場合に使う。
void GenerateAnchorIdInto(std::string_view text, std::pmr::string& slug);

// 拡張子の大文字小文字は区別しない。
bool IsMarkdownFile(std::wstring_view path);

inline constexpr std::wstring_view HELP_PATH = L"mendo://help";
inline constexpr bool IsHelpPath(std::wstring_view path) noexcept
{
    return path == HELP_PATH;
}

std::pmr::wstring ExtractFilename(std::wstring_view path);

// zoom_percent: 0 または 100 はデフォルト（省略）、それ以外は "(125%)" 等として表示。
std::pmr::wstring BuildTitleString(std::wstring_view path, int zoom_percent = 0);
