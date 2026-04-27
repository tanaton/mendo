#pragma once
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string_view>

// 新旧コンテンツの最初の差分バイトオフセットを返す。同一の場合は npos。
[[nodiscard]] size_t FindFirstDifference(std::string_view old_text, std::string_view new_text) noexcept;

// diff_pos が短い方の末尾と一致するかを判定する。
// 片方がもう片方の prefix であり、ファイルの伸縮（エディタの中間書き込み状態）を示す。
[[nodiscard]] inline bool IsPrefixOnlyDiff(size_t diff_pos, size_t old_size, size_t new_size) noexcept
{
    return diff_pos == std::min(old_size, new_size);
}

// 同一ファイルのリロード時に取るべき操作を示す。
enum class ReloadOp : uint8_t {
    NoChange,           // 差分なし。リロード不要。
    DeferPrefixShrink,  // prefix-only shrink。truncate→rewrite の前半として defer。
    PrefixGrowth,       // prefix-only growth。レイアウトキャッシュの prefix を保存して伸張。
    FullReload,         // 全体差分リロード。
};

struct ReloadDecision {
    ReloadOp op;
    size_t diff_pos;    // NoChange のとき std::string_view::npos、それ以外は差分開始位置。
};

// 旧/新コンテンツの UTF-8 バイト列を比較し、リロード方針を決定する純粋関数。
// FindFirstDifference + IsPrefixOnlyDiff + shrink 判定を 1 か所に集約し、
// OnParseComplete / DoReloadCurrentFile の同一ファイル再読み込み時の分岐を統一する。
[[nodiscard]] ReloadDecision AnalyzeReloadDiff(std::string_view old_utf8, std::string_view new_utf8) noexcept;
