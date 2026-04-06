#pragma once
#include "types.h"
#include <string>
#include <string_view>
#include <vector>
#include <memory_resource>

// パーサーの出力: ノード本体と、パース中に構築された特殊ノードインデックス
struct ParseResult {
    std::pmr::vector<Node> nodes;
    std::pmr::vector<size_t> heading_indices;
    std::pmr::vector<size_t> image_indices;
    std::pmr::vector<size_t> mermaid_indices;
};

[[nodiscard]] ParseResult ParseMarkdown(std::string_view markdown_text);

// 見出しテキストからGitHubスタイルのアンカースラグを生成する（テスト用に公開）
[[nodiscard]] std::pmr::wstring GenerateAnchorId(std::wstring_view text);

// BlockQuoteノードからGitHub Alertsを検出し、マーカー除去・ラベル挿入・グルーピングを行う（テスト用に公開）
void DetectAlerts(std::pmr::vector<Node>& nodes);

// AlertTypeに対応するラベル文字列を返す（テスト用に公開）
[[nodiscard]] const wchar_t* GetAlertLabel(AlertType type) noexcept;

// AlertTypeに対応するアイコン文字列を返す（テスト用に公開）
[[nodiscard]] const wchar_t* GetAlertIcon(AlertType type) noexcept;
