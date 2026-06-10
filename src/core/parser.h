#pragma once
#include "document_types.h"
#include <optional>
#include <span>
#include <stop_token>
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
    std::pmr::vector<size_t> table_indices;
};

// 本物の Markdown パーサ。md4c を UTF-8 モード (MD_CHAR=char) で起動するため入力は char 列。
// stop_token を渡すと md4c コールバックから途中で abort できる。default-constructed の場合は
// 永久に stop_requested() == false なので、従来どおり最後まで走る。
ParseResult ParseMarkdown(std::string_view markdown_text, std::stop_token stop_token = {});

// BlockQuoteノードからGitHub Alertsを検出し、マーカー除去・ラベル挿入・グルーピングを行う（テスト用に公開）。
// blockquote_indices は ParseMarkdown が収集した BlockQuote ノードのインデックス。
void DetectAlerts(std::pmr::vector<Node>& nodes, std::span<const size_t> blockquote_indices);

// AlertTypeに対応するラベル文字列を返す（テスト用に公開）
std::string_view GetAlertLabel(AlertType type) noexcept;

// AlertTypeに対応するアイコン文字列を返す（テスト用に公開）
std::string_view GetAlertIcon(AlertType type) noexcept;

// HTML エンティティ (例: "&amp;", "&#x1F600;") を解決する。
// 戻り値: 解決成功なら char 文字列の view、失敗なら nullopt (呼び出し側で元の入力を
// そのままテキストとして再投入することを示す)。
// view が指す領域は (a) static なリテラル または (b) 呼び出し側が渡した buffer のいずれか。
// buffer のスコープ内でのみ valid。UTF-8 では U+10FFFF までを最大 4 byte で表現できる。
[[nodiscard]] std::optional<std::string_view> ResolveHtmlEntity(std::string_view entity, char (&buffer)[4]);
