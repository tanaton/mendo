#pragma once
#include "document_types.h"
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

struct WordBoundary {
    uint32_t start = 0;
    uint32_t end = 0;
    bool found = false;
};

// カテゴリ: 語 (英数+_・ラテン文字・ギリシャ文字・キリル文字・ハングル) / ひらがな / カタカナ / 漢字 / 全角英数 / その他。
// 「その他」(空白・記号など) は {0, 0, false}。形態素解析は行わない (UAX#29 簡易版)。
// pos がマルチバイト/サロゲートの途中を指した場合は code point 先頭へスナップする。
WordBoundary FindWordBoundaries(std::string_view text, uint32_t pos) noexcept;
WordBoundary FindWordBoundaries(std::wstring_view text, uint32_t pos) noexcept;

// nodes[node] の pos を含む語。パーサが改行を挟まずに区切った継ぎ目で切れた語は、隣のノードへ伸ばす。
struct NodeWordRange {
    size_t start_node = 0;
    uint32_t start = 0;
    size_t end_node = 0;
    uint32_t end = 0;
    bool found = false;
};
NodeWordRange FindNodeWordBoundaries(std::span<const Node> nodes, size_t node, uint32_t pos) noexcept;

// pos を含む run。runs は start 昇順で重ならない。無ければ null。
const TextRun* FindRunAt(std::span<const TextRun> runs, uint32_t pos) noexcept;

// 数式の run の前後にある区切り ($ か $$) の長さ。数式は区切りの内側 (TeX) だけを描いてコピーする。
// 数式でない run と、区切りがそろっていない run (長い本文を区切って片側の $$ が無いもの) は 0 で、原文のまま扱う。
constexpr uint32_t MathDelimiterLength(std::string_view text, const TextRun& run) noexcept
{
    if (!run.math()) {
        return 0;
    }
    const std::string_view delim = run.double_dollar_math() ? "$$" : "$";
    const std::string_view whole = text.substr(run.start, run.length);
    const bool balanced = whole.size() > delim.size() * 2 && whole.starts_with(delim) && whole.ends_with(delim);
    return balanced ? static_cast<uint32_t>(delim.size()) : 0;
}

// $$..$$ だけの段落と ```math (パーサが $$ で囲む)。段落いっぱいの数式を 1 つだけ持つ。
// 長い $$..$$ を区切った途中のノードも run が段落いっぱいになるが、区切りがそろわないので数式ではない。
constexpr bool IsBlockMath(const Node& node) noexcept
{
    const auto runs = node.runs();
    const std::string_view text = node.GetText();
    return runs.size() == 1 && runs[0].double_dollar_math() && runs[0].start == 0 && runs[0].length == text.size() &&
           MathDelimiterLength(text, runs[0]) > 0;
}

// node の pos にある run の並びと、その中での位置。表はセルごとに run を持ち、pos は linearized 形式 (concat_text) の
// offset。セル外 (区切り文字・padding) なら runs は空。
struct NodeRunsAt {
    std::span<const TextRun> runs;
    uint32_t local_pos = 0;
};
NodeRunsAt FindRunsAt(const Node& node, uint32_t pos) noexcept;

// CRLF / 旧式 CR を LF に正規化する (in-place)。
// 改行を LF に揃えておくと、パーサ中の current_text と DocumentData::raw の memcmp 一致判定が成立し、
// 大半の code block が DocumentData::raw をそのまま指せる (arena へのコピーが要らない)。
// 複数行の paragraph は SOFTBR を空白に置き換えるため常に arena へコピーする。
// CR まで一気にスキップし、その間は memmove でブロックコピーする (MSVC UCRT で SIMD)。
// LF-only ファイルでは memchr 1 回で素通し。
void NormalizeNewlines(std::string& s);

// リロードで新旧のノード列の先頭 head 個と末尾 tail 個が、表示に効く中身で一致した範囲 (両者は重ならない)。
// ソース位置と引用グループ番号は比べない。
// 編集は 1 か所に固まることが多いので、先頭と末尾から一致する範囲だけを見る。
struct UnchangedEnds {
    size_t head = 0;
    size_t tail = 0;
    size_t old_count = 0;
    size_t new_count = 0;

    constexpr size_t OldTailBegin() const noexcept
    {
        return old_count - tail;
    }
    constexpr size_t NewTailBegin() const noexcept
    {
        return new_count - tail;
    }
};
UnchangedEnds FindUnchangedEnds(std::span<const Node> old_nodes, std::span<const Node> new_nodes) noexcept;

// コードブロックのトークン。区切ったコードの続きは、前のノードから続くトークン (code_lead_length) を先頭に置き、
// その後ろからトークン化する (ブロックコメントや複数行の文字列の途中から始まっても色が崩れないように)。
std::vector<SyntaxToken> TokenizeCodeNode(const Node& node);

// 区切ったコードブロックの並び (先頭から末尾まで) の続きのノードに、前のノードから続くトークンを記録する。
// 並びをつないだテキストを数ノードずつの窓で字句解析し、窓の端で切れたトークンはその先頭から次の窓で解析し直す。
// 並び全体のトークンを一度に持たない。
void AnnotateCodeChunkLeads(std::span<Node> run);

// source_offset が diff_offset 以下の最後のノードを返す。該当なしの場合は -1。
[[nodiscard]] int FindNodeBySourceOffset(const std::vector<Node>& nodes, size_t diff_offset) noexcept;
