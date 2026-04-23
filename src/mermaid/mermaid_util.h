#pragma once
#include <string>
#include <string_view>
#include <cstdint>

struct Node;

namespace mermaid_util {

// LaTeX 原文を mermaid flowchart の1ノードラベルでラップする。
// style でノードの枠と背景を非表示にし、数式のみが見えるようにする。
std::pmr::wstring BuildLatexFlowchartCode(std::wstring_view latex);

// ダイアグラムノード用キャッシュキー。言語種別に応じたソルトを内部で混ぜ、
// 同じ UTF-8 コンテンツの Mermaid / LatexMath がキー衝突しないようにする。
uint64_t NodeDiagramHash(const Node& node, float max_width, bool dark_mode) noexcept;

std::pmr::wstring JsEscape(std::wstring_view input);

std::pmr::wstring SimpleHash(std::wstring_view input);

uint64_t HashRaw(std::wstring_view input) noexcept;
uint64_t HashRaw(std::string_view input) noexcept;

uint64_t CombinedHash(std::wstring_view code, int max_width_int, bool dark_mode) noexcept;
uint64_t CombinedHash(std::string_view code, int max_width_int, bool dark_mode) noexcept;

int ComputeWorkerCount(unsigned int processor_count) noexcept;
int QuantizeWidth(float max_width) noexcept;
uint64_t HashCode(std::wstring_view code, float max_width, bool dark_mode) noexcept;
uint64_t HashCode(std::string_view code, float max_width, bool dark_mode) noexcept;
float ParseJsonNumber(std::wstring_view json, std::wstring_view key) noexcept;
bool ParseJsonTrueFlag(std::wstring_view json, std::wstring_view key) noexcept;

struct RequestPrefix {
    unsigned int id = 0;
    std::wstring_view payload;
    bool valid = false;
    bool has_payload = false;
};
RequestPrefix ParseRequestPrefix(std::wstring_view body) noexcept;
}
