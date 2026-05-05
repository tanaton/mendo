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

// 本物の Markdown パーサ。入力は UTF-16 wide。内部で per-parse 1 回だけ UTF-8 化し
// md4c (UTF-8 ビルド) に渡す。view 化判定や source_offset は引き続き UTF-16 単位。
ParseResult ParseMarkdown(std::wstring_view markdown_text);

// utf8 と wide の両方を呼び出し側が既に持っている場合の高速パス (BuildUtf8FromWide を
// スキップ)。Document::FromMarkdown(utf8) のように元の utf8 を保持できる経路で利用する。
// 不変条件: markdown_utf8 は markdown_text を WideCharToMultiByte(CP_UTF8) で得たものと
// バイト単位で一致すること (改行正規化後)。
ParseResult ParseMarkdown(std::wstring_view markdown_text, std::string_view markdown_utf8);

// BlockQuoteノードからGitHub Alertsを検出し、マーカー除去・ラベル挿入・グルーピングを行う（テスト用に公開）。
// blockquote_indices は ParseMarkdown が収集した BlockQuote ノードのインデックス。
void DetectAlerts(std::pmr::vector<Node>& nodes, std::span<const size_t> blockquote_indices);

// AlertTypeに対応するラベル文字列を返す（テスト用に公開）
std::wstring_view GetAlertLabel(AlertType type) noexcept;

// AlertTypeに対応するアイコン文字列を返す（テスト用に公開）
std::wstring_view GetAlertIcon(AlertType type) noexcept;

// HTML エンティティ (例: L"&amp;", L"&#x1F600;") を解決する。
// 戻り値: 解決成功なら wide 文字列の view、失敗なら nullopt (呼び出し側で元の入力を
// そのままテキストとして再投入することを示す)。
// view が指す領域は (a) static なリテラル または (b) 呼び出し側が渡した buffer のいずれか。
// buffer のスコープ内でのみ valid。
[[nodiscard]] std::optional<std::wstring_view> ResolveHtmlEntity(std::wstring_view entity, wchar_t (&buffer)[2]);
