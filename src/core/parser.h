#pragma once
#include "document_types.h"
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>
#include <memory_resource>

// パーサーの出力: ノード本体と、パース中に構築された特殊ノードインデックス
struct ParseResult {
    std::pmr::vector<Node> nodes;
    std::pmr::vector<size_t> heading_indices;
    std::pmr::vector<size_t> image_indices;
    std::pmr::vector<size_t> diagram_indices;
};

// 本物の Markdown パーサ。md4c を MD4C_USE_UTF16/UTF-8 モードで起動するため入力は doc_char 列。
ParseResult ParseMarkdown(mendo::doc_string_view markdown_text);

// BlockQuoteノードからGitHub Alertsを検出し、マーカー除去・ラベル挿入・グルーピングを行う（テスト用に公開）。
// blockquote_indices は ParseMarkdown が収集した BlockQuote ノードのインデックス。
void DetectAlerts(std::pmr::vector<Node>& nodes, std::span<const size_t> blockquote_indices);

// AlertTypeに対応するラベル文字列を返す（テスト用に公開）
mendo::doc_string_view GetAlertLabel(AlertType type) noexcept;

// AlertTypeに対応するアイコン文字列を返す（テスト用に公開）
mendo::doc_string_view GetAlertIcon(AlertType type) noexcept;

// HTML エンティティ (例: "&amp;", "&#x1F600;") を解決する。
// 戻り値: 解決成功なら doc_char 文字列の view、失敗なら nullopt (呼び出し側で元の入力を
// そのままテキストとして再投入することを示す)。
// view が指す領域は (a) static なリテラル または (b) 呼び出し側が渡した buffer のいずれか。
// buffer のスコープ内でのみ valid。
// UTF-16 ビルドでは buffer は最大 2 code unit (BMP 1 + サロゲートペア 2)、
// UTF-8 ビルドでは最大 4 byte (U+10FFFF まで)。両ビルドで足りる 4 を採用する。
[[nodiscard]] std::optional<mendo::doc_string_view> ResolveHtmlEntity(mendo::doc_string_view entity, mendo::doc_char (&buffer)[4]);
