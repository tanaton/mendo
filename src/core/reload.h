#pragma once
// 同一ファイルのリロード判定ロジック。
// 旧/新コンテンツの UTF-8 テキストを比較し、リロード方針を決定する純粋関数群と、
// 差分位置 (UTF-8 byte オフセット) を対応するノード / スクロール Y 座標へ写像する
// ルックアップを提供する。OnParseComplete / DoReloadCurrentFile の同一ファイル
// 再読み込み時の分岐を統一するために 1 か所に集約している。
#include "document_types.h"
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory_resource>
#include <string_view>
#include <vector>

class LayoutCache;
struct Theme;

// 新旧コンテンツの最初の差分 UTF-8 byte 位置を返す。同一の場合は npos。
size_t FindFirstDifference(mendo::doc_string_view old_text, mendo::doc_string_view new_text) noexcept;

// diff_pos が短い方の末尾と一致するかを判定する。
// 片方がもう片方の prefix であり、ファイルの伸縮（エディタの中間書き込み状態）を示す。
inline constexpr bool IsPrefixOnlyDiff(size_t diff_pos, size_t old_size, size_t new_size) noexcept
{
    return diff_pos == std::min(old_size, new_size);
}

// 同一ファイルのリロード時に取るべき操作を示す。
enum class ReloadOp : uint8_t {
    NoChange,          // 差分なし。リロード不要。
    DeferPrefixShrink, // prefix-only shrink。truncate→rewrite の前半として defer。
    PrefixGrowth,      // prefix-only growth。レイアウトキャッシュの prefix を保存して伸張。
    FullReload,        // 全体差分リロード。
};

struct ReloadDecision {
    ReloadOp op;
    size_t diff_pos; // NoChange のとき mendo::doc_string_view::npos、それ以外は差分開始位置。
};

// 旧/新コンテンツの UTF-8 テキストを比較し、リロード方針を決定する純粋関数。
ReloadDecision AnalyzeReloadDiff(mendo::doc_string_view old_text, mendo::doc_string_view new_text) noexcept;

// source_offset が diff_offset 以下の最後のノードを返す。該当なしの場合は -1。
[[nodiscard]] int FindNodeBySourceOffset(const std::pmr::vector<Node>& nodes, uint32_t diff_offset) noexcept;

// diff 位置のノードに基づくスクロールY座標を計算する。
// ノードが見つからない場合は fallback_scroll を返す。
// theme はノードのテキスト上端 Y を Fenwick から取り出すために必要。
float CalcScrollYForDiff(
    const std::pmr::vector<Node>& nodes,
    const LayoutCache& cache,
    const Theme& theme,
    mendo::doc_string_view content,
    size_t diff_pos,
    float viewport_height,
    float fallback_scroll) noexcept;
