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

// wstringをJavaScript文字列リテラルとして安全に埋め込むためにエスケープする。
std::pmr::wstring JsEscape(std::wstring_view input);

// FNV-1a 64ビットハッシュ。16文字の16進数wstringとして返す。
std::pmr::wstring SimpleHash(std::wstring_view input);

// FNV-1a 64ビットハッシュの生の値を返す。
uint64_t HashRaw(std::wstring_view input) noexcept;
uint64_t HashRaw(std::string_view input) noexcept;

// 複数の値からキャッシュキーのハッシュを計算する（コード全体のコピーを回避）。
uint64_t CombinedHash(std::wstring_view code, int max_width_int, bool dark_mode) noexcept;
uint64_t CombinedHash(std::string_view code, int max_width_int, bool dark_mode) noexcept;

// 論理プロセッサ数からMermaidレンダリング用ワーカー数を計算する。
// 結果は [2, 4] にクランプされる。
int ComputeWorkerCount(unsigned int processor_count) noexcept;

// 幅を100px単位に量子化する（ファイルキャッシュのキー用）。
// 結果は常に100以上の100の倍数。
int QuantizeWidth(float max_width) noexcept;

// 幅を量子化してからキャッシュキーのハッシュを計算する。
uint64_t HashCode(std::wstring_view code, float max_width, bool dark_mode) noexcept;
uint64_t HashCode(std::string_view code, float max_width, bool dark_mode) noexcept;
}
