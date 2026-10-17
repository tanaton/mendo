#pragma once
#include "document_types.h"
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

std::pmr::string ToLowerAsciiCopy(std::string_view text);

// GitHub スタイル: 小文字化、空白→'-'、CJK 保持、句読点スキップ。結果を slug に書く。
void GenerateAnchorIdInto(std::string_view text, std::pmr::string& slug);

// リンク先のアンカーを GenerateAnchorIdInto と同じ規則で小文字にする。
std::pmr::string ToLowerAnchorCopy(std::string_view text);

// 表示に効く中身 (種別・テキスト・runs・リンク先・入れ子の深さ・表や画像など) が同じか。
// ソース位置と引用グループ番号は比べない。リロードで計測結果を使い回せるかの判定に使う。
bool NodeContentEquals(const Node& a, const Node& b) noexcept;

// 最後のパス区切り以降の "." を含む拡張子。無ければ空。
std::wstring_view ExtensionView(std::wstring_view path) noexcept;

// 拡張子の大文字小文字は区別しない。
bool IsMarkdownFile(std::wstring_view path);

inline constexpr std::wstring_view HELP_PATH = L"mendo://help";
inline constexpr bool IsHelpPath(std::wstring_view path) noexcept
{
    return path == HELP_PATH;
}

std::pmr::wstring ExtractFilename(std::wstring_view path);

// 空パスや区切りを含まないパスでは空を返す。
std::pmr::wstring ParentDirectory(std::wstring_view path);

// zoom_percent: 0 または 100 はデフォルト（省略）、それ以外は "(125%)" 等として表示。
std::pmr::wstring BuildTitleString(std::wstring_view path, int zoom_percent = 0);
