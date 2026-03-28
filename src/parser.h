#pragma once
#include "types.h"
#include <string>
#include <string_view>
#include <vector>
#include <memory_resource>

[[nodiscard]] std::pmr::vector<Node> ParseMarkdown(std::string_view markdown_text);

// 見出しテキストからGitHubスタイルのアンカースラグを生成する（テスト用に公開）
[[nodiscard]] std::pmr::wstring GenerateAnchorId(std::wstring_view text);

// BlockQuoteノードからGitHub Alertsを検出し、マーカー除去・ラベル挿入・グルーピングを行う（テスト用に公開）
void DetectAlerts(std::pmr::vector<Node>& nodes);

// AlertTypeに対応するラベル文字列を返す（テスト用に公開）
[[nodiscard]] const wchar_t* GetAlertLabel(AlertType type) noexcept;

// AlertTypeに対応するアイコン文字（Segoe Fluent Icons）を返す（テスト用に公開）
[[nodiscard]] wchar_t GetAlertIcon(AlertType type) noexcept;
