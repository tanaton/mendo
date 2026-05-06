#pragma once
#include "document_types.h"
#include "reload.h"
#include "selection_html.h"
#include <cstdint>
#include <memory_resource>
#include <string>
#include <string_view>

// ダブルクリックによる単語選択の単語境界結果。
struct WordBoundary {
    uint32_t start = 0;
    uint32_t end = 0;
    bool found = false;
};

// テキスト内の指定位置周辺の単語境界を検索する。
// 「単語文字」は英数字またはアンダースコア。
// 位置が単語文字上にあれば {start, end, true} を返し、そうでなければ {0, 0, false} を返す。
WordBoundary FindWordBoundaries(std::string_view text, uint32_t pos) noexcept;

// ASCII範囲の大文字を小文字に変換する。
std::pmr::string ToLowerAscii(std::string_view text);

// 見出しテキストからGitHubスタイルのアンカースラグを生成する。
// ASCII は小文字化、空白は '-'、CJK 文字は保持しつつ句読点・記号はスキップする。
std::pmr::string GenerateAnchorId(std::string_view text);

// アロケータ指定済みの既存 slug に書き込むバリアント。呼び出し側で allocator を揃えたい場合に使う。
void GenerateAnchorIdInto(std::string_view text, std::pmr::string& slug);

// ファイルパスまたはファイル名がMarkdownファイル（.md, .markdown, .mkd）かどうかを判定する。
// 拡張子の大文字小文字は区別しない。
bool IsMarkdownFile(std::wstring_view path);

// ヘルプ用仮想パス
inline constexpr std::wstring_view HELP_PATH = L"mendo://help";
inline constexpr bool IsHelpPath(std::wstring_view path) noexcept
{
    return path == HELP_PATH;
}

// フルファイルパスからファイル名部分を抽出する。
// 例: "C:\\dir\\file.md" -> "file.md"
std::pmr::wstring ExtractFilename(std::wstring_view path);

// ファイルパスからタイトル文字列を構築する。
// 例: "C:\\dir\\file.md" -> "file.md - mendo"
// パスが空の場合は "mendo" を返す。
// zoom_percent: 0 または 100 はデフォルト（省略）、それ以外は "(125%)" 等として表示される。
std::pmr::wstring BuildTitleString(std::wstring_view path, int zoom_percent = 0);
