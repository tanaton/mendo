#pragma once
#include "document_types.h"
#include "text_arena.h"
#include <cstddef>
#include <memory_resource>
#include <span>
#include <string_view>
#include <vector>

// BlockQuoteノードからGitHub Alertsを検出し、マーカー除去・ラベル挿入・グルーピングを行う。
// blockquote_indices は ParseMarkdown が収集した BlockQuote ノードのインデックス。書き換えたテキストと runs は arena に置く。
void DetectAlerts(std::pmr::vector<Node>& nodes, TextArena& arena, std::span<const size_t> blockquote_indices);

// text が Alert マーカー ([!TYPE] と直後の区切り 1 文字) だけからなるか。
bool IsAlertMarkerOnly(std::string_view text);

// nodes[i] が Alert マーカーを持ちうる位置 (最外側 blockquote の先頭段落) か。
bool IsAlertHeadCandidate(const std::pmr::vector<Node>& nodes, size_t i) noexcept;

std::string_view GetAlertLabel(AlertType type) noexcept;
std::string_view GetAlertIcon(AlertType type) noexcept;
