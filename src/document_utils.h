#pragma once
#include "types.h"
#include <string>
#include <string_view>
#include <vector>
#include <optional>
#include <memory_resource>

// 選択範囲に基づいてノードから選択テキストを抽出する。
// ノード間を \r\n で連結したテキストを返す。
[[nodiscard]] std::pmr::wstring ExtractSelectedText(const std::pmr::vector<Node>& nodes,
                                  const TextSelection& selection);

// ノードのラン内の指定テキスト位置にあるリンクURLを検索する。
// リンクラン内の位置であればリンクURLを返し、そうでなければnulloptを返す。
[[nodiscard]] std::optional<std::pmr::wstring> FindLinkAtPosition(const Node& node, uint32_t text_pos);

// 指定されたアンカーIDに一致する見出しノードのインデックスを検索する。
// アンカーは大文字小文字を区別せず（小文字化して）比較される。
// 見つからない場合は-1を返す。
int FindAnchorNodeIndex(const std::pmr::vector<Node>& nodes, std::wstring_view anchor);

// ダブルクリックによる単語選択の単語境界結果。
struct WordBoundary {
    uint32_t start = 0;
    uint32_t end = 0;
    bool found = false;
};

// テキスト内の指定位置周辺の単語境界を検索する。
// 「単語文字」は英数字またはアンダースコア。
// 位置が単語文字上にあれば {start, end, true} を返し、そうでなければ {0, 0, false} を返す。
WordBoundary FindWordBoundaries(std::wstring_view text, uint32_t pos);

// ASCII範囲の大文字を小文字に変換する。
[[nodiscard]] std::pmr::wstring ToLowerAscii(std::wstring_view text);

// ファイルパスまたはファイル名がMarkdownファイル（.md, .markdown, .mkd）かどうかを判定する。
// 拡張子の大文字小文字は区別しない。
[[nodiscard]] bool IsMarkdownFile(std::wstring_view path);

// フルファイルパスからファイル名部分を抽出する。
// 例: "C:\\dir\\file.md" -> "file.md"
[[nodiscard]] std::pmr::wstring ExtractFilename(std::wstring_view path);

// ファイルパスからタイトル文字列を構築する。
// 例: "C:\\dir\\file.md" -> "file.md - mendo"
// パスが空の場合は "mendo" を返す。
// zoom_percent: 0 または 100 はデフォルト（省略）、それ以外は "(125%)" 等として表示される。
[[nodiscard]] std::pmr::wstring BuildTitleString(std::wstring_view path, int zoom_percent = 0);
