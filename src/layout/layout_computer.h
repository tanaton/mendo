#pragma once
#include "document_types.h"
#include "document_utils.h"
#include "layout_cache.h"
#include "theme.h"
#include "ui_constants.h"
#include <algorithm>
#include <limits>
#include <memory_resource>
#include <span>
#include <stop_token>
#include <string_view>

namespace layout {

inline float NodeIndent(const Node& node, const Theme& theme) noexcept
{
    return node.indent_level * theme.indent_width;
}

// ブロック左端のウィンドウ DIP X (横スクロール前)。
inline float NodeBlockX(const Node& node, const Theme& theme) noexcept
{
    return theme.margin_left + NodeIndent(node, theme);
}

// インデントは 127 段まで積めるので、本文幅から引くだけだと負になる。
inline float NodeContentWidth(const Node& node, const Theme& theme, float content_width) noexcept
{
    return ShrinkKeepingMinContent(content_width, NodeIndent(node, theme));
}

// 折り返さないノード (CodeBlock) を組む幅。高さも行も本文幅に依存しない。
// 1e7 は float の整数精度限界 (2^24 ≈ 1.67e7) より少し下で、DirectWrite 内部の
// 幾何計算でも丸め誤差が乗らない安全な値。10MDIP ≈ 数十万行のコードブロックを許容する。
// MaxHeight も事実上の上限で十分なので同じ値を使う。
inline constexpr float kNoWrapLayoutWidth = 1.0e7f;

// ノードの text_layout を組む幅。計測結果を使い回せるかはこの幅が同じかで決まる。
inline float NodeLayoutWidth(const Node& node, float node_width) noexcept
{
    return node.type == NodeType::CodeBlock ? kNoWrapLayoutWidth : node_width;
}

inline float NodeTextXOffset(const Node& node, const Theme& theme) noexcept
{
    return (node.type == NodeType::CodeBlock) ? theme.code_block_padding : 0.0f;
}

// ブロック背景がテキスト範囲の上と下にはみ出す量。背景・ヒット判定・横スクロールバー位置で共有する。
// 区切ったコードブロックの継ぎ目でははみ出さず、隣のノードの背景とつなげる。
inline float NodeBoxPadTop(const Node& node, const Theme& theme) noexcept
{
    return (node.type == NodeType::CodeBlock && !node.continues_prev()) ? theme.code_block_padding : 0.0f;
}
inline float NodeBoxPadBottom(const Node& node, const Theme& theme) noexcept
{
    return (node.type == NodeType::CodeBlock && !node.continues_next()) ? theme.code_block_padding : 0.0f;
}

// ダイアグラム/画像ノードのビットマップ未確定時に使うプレースホルダー高さ。
inline float PlaceholderHeight(const Theme& theme) noexcept
{
    return std::max(MIN_DIAGRAM_PLACEHOLDER_HEIGHT, theme.font_size_body * 3.0f);
}

// 子ブロックも無い空 LI は bullet / checkbox だけの 1 行。
inline float EmptyListItemHeight(const Theme& theme) noexcept
{
    return theme.font_size_body * std::max(TASK_CHECKBOX_HEIGHT_FACTOR, FALLBACK_LINE_HEIGHT_FACTOR);
}

// テキストが空のノードの高さ。中身を子ブロックに持つ LI は 0 にする
// (paragraph_spacing を入れると bullet と直下 P の文字 Y が分離する issue#237)。
inline float EmptyTextNodeHeight(const Node& node, const Theme& theme) noexcept
{
    return IsEmptyListItemContainer(node) ? 0.0f : IsListItem(node) ? EmptyListItemHeight(theme) : theme.paragraph_spacing;
}

inline float HorizontalRuleHeight(const Theme& theme) noexcept
{
    return theme.paragraph_spacing + theme.hr_thickness;
}

// 行高さが未計測の表の行に使う高さ。
inline float TableBaseRowHeight(const Theme& theme) noexcept
{
    return theme.font_size_body * TABLE_ROW_HEIGHT_FACTOR;
}

constexpr float BitmapDisplayHeight(float width, float height, float max_width) noexcept
{
    return FitBitmapSize(width, height, max_width).height;
}

// ノードの右上に並ぶオーバーレイボタンを fn(rect, kind) で列挙する。描画とヒットテストで矩形を共有する。
// x / w はノードの左端と内容幅、top はテキスト上端。diagram は図の結果 (コピー・保存する PNG が無ければボタンも無い)。
template <class Fn>
void ForEachOverlayButton(const Node& node, const DiagramEntry* diagram, float x, float w, float top, const Theme& theme, Fn&& fn)
{
    // 区切ったコードブロックは先頭にだけ置き、並び全体をコピーする。
    if (node.type == NodeType::CodeBlock && !node.continues_prev()) {
        fn(OverlayButtonRect(x + w, top - NodeBoxPadTop(node, theme), OverlayButton::CodeCopy), OverlayButton::CodeCopy);
    }
    else if (node.type == NodeType::Diagram && diagram && diagram->png) {
        const auto bmp = MermaidBitmapRect(diagram->width, diagram->height, x, w, top);
        for (const auto kind : { OverlayButton::DiagramSave, OverlayButton::DiagramCopy }) {
            fn(OverlayButtonRect(bmp.right, bmp.top, kind), kind);
        }
    }
}

// ビットマップを描くノードの高さ。寸法が届くまでは現在値 (推定・前回値) を保ち、それも無ければプレースホルダー。
inline float BitmapNodeHeight(const DiagramEntry* bitmap, float node_width, float current_height, const Theme& theme) noexcept
{
    if (bitmap && bitmap->width > 0 && bitmap->height > 0) {
        return BitmapDisplayHeight(bitmap->width, bitmap->height, node_width);
    }
    return current_height > 0 ? current_height : PlaceholderHeight(theme);
}

float GetSpacingAbove(const Node& node, const Theme& theme) noexcept;
float GetSpacingBelow(const Node& node, const Theme& theme) noexcept;

// 1 ノード分の Y 進行。戻り値はテキスト上端。加算順序 (above → height → below) は大規模ファイルでの
// catastrophic cancellation 回避のため既存の累積順序を厳密に保持する。
inline float AdvanceNodeY(float& y, float spacing_above, float height, float spacing_below) noexcept
{
    y += spacing_above;
    const float text_top = y;
    y += height;
    y += spacing_below;
    return text_top;
}

void ComputeColumnWidths(std::pmr::vector<float>& out, std::span<const float> natural_widths, float available_width);

// node_width > 0 なら、折り返す段落の行数を文字数から見積もる。0 は幅を知らないときの推定 (起動時の先読み)。
float EstimateNodeHeight(const Node& node, const Theme& theme, float node_width) noexcept;

// content_width > 0 なら折り返しを見積もる。stop_token が stop_requested になると途中 return する (cache は中間状態のまま)。
void EstimateNodeHeights(const std::pmr::vector<Node>& nodes, LayoutCache& cache, const Theme& theme,
                         float content_width, std::stop_token stop_token);

// 全ノードを dirty にして高さを推定値で更新し (EstimateInvisibleNodeHeight)、Y を組み直す。
void ReestimateNodeHeights(const std::pmr::vector<Node>& nodes, LayoutCache& cache, const Theme& theme, float content_width);

// リロードで、中身が変わらないノード (FindUnchangedEnds) の計測結果を旧キャッシュから新キャッシュへ移す。
// 変わった範囲の図は、新しい結果が届くまで旧結果を仮表示として引き継ぐ。
// Y 位置は呼び出し側が組み直す。
struct CarryOverResult {
    UnchangedEnds ends;
    size_t carried = 0; // 移したノード数 (仮表示の図を含む)
};
// ends は FindUnchangedEnds(old_nodes, new_nodes) の結果。
CarryOverResult CarryOverUnchanged(
    const std::pmr::vector<Node>& old_nodes, LayoutCache& old_cache,
    const std::pmr::vector<Node>& new_nodes, LayoutCache& new_cache, const UnchangedEnds& ends);

// 新文書用の LayoutCache。未計測のまま位置が全て 0 だと可視範囲の判定が効かないため、
// 文書を表示に載せる経路はすべてこれで作る。stop 時は中間状態なので呼び出し側で破棄する。
// 表示する本文幅を渡せば、UI スレッドで推定し直さずに済む (0 は幅を知らない)。
LayoutCache MakeEstimatedLayoutCache(const std::pmr::vector<Node>& nodes, const Theme& theme,
                                     float content_width = 0.0f, std::stop_token stop_token = {});

// 不可視ノードに対し、現在の高さを下回らない範囲で推定値で更新する。
// 型別の touch/no-touch ポリシー (Diagram は触らない、Table は推定で成長させた場合のみ
// table_layout を invalidate) を内部に閉じ込める。戻り値: 高さが更新されたら true。
bool EstimateInvisibleNodeHeight(const Node& node, NodeLayoutEntry& entry, const Theme& theme, float node_width) noexcept;

// 高さが変わったノードの閉区間 [first, last]。以降のノードは一定量のシフトで済むため、
// 再計算を先頭からの全件ではなくこの範囲に限定できる。既定値は空。
struct HeightChangeRange {
    size_t first = std::numeric_limits<size_t>::max();
    size_t last = 0;

    constexpr bool empty() const noexcept
    {
        return first > last;
    }
    constexpr void Add(size_t i) noexcept
    {
        if (empty()) {
            first = last = i;
            return;
        }
        first = std::min(first, i);
        last = std::max(last, i);
    }
};

void RecomputeYPositions(
    const std::pmr::vector<Node>& nodes, LayoutCache& cache, const Theme& theme,
    size_t from_index = 0, size_t safe_exit_after = std::numeric_limits<size_t>::max()) noexcept;

// scroll_y + margin の位置にある見出し (目次の項目番号)。無ければ -1。
// margin は見出し上部余白で、画面上端に近い見出しを正しくアクティブにする。
int FindActiveTocIndex(std::span<const TocEntry> entries, const LayoutCache& cache, float scroll_y, float margin) noexcept;

// diff 位置のノードに基づくスクロールY座標を計算する。
// diff_pos が npos (差分位置なし = スクロール維持) またはノードが見つからない場合は
// fallback_scroll を返す。
float CalcScrollYForDiff(
    const std::pmr::vector<Node>& nodes,
    const LayoutCache& cache,
    std::string_view content,
    size_t diff_pos,
    float viewport_height,
    float fallback_scroll) noexcept;

} // namespace layout
