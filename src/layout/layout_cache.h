#pragma once
#include "document_types.h"
#include <vector>
#include <memory_resource>
#include <wrl/client.h>
#include <d2d1.h>
#include <dwrite.h>
#include <algorithm>
#include <cassert>
#include <ranges>


// インラインコードの背景矩形（レイアウト原点からの相対座標、パディング適用済み）
using InlineCodeBg = D2D1_RECT_F;

// テーブル専用のレイアウトデータ（テーブルノードのみ確保してメモリを節約）
struct TableLayoutData {
    std::pmr::vector<Microsoft::WRL::ComPtr<IDWriteTextLayout>> cell_layouts; // フラット配列 [行 * col_count + 列]
    std::pmr::vector<std::pmr::vector<InlineCodeBg>> cell_inline_code_bgs; // フラット配列 [行 * col_count + 列][]
    size_t col_count = 0; // cell_layoutsのストライド
    std::pmr::vector<float> col_widths;
    std::pmr::vector<float> row_heights;
    std::pmr::vector<float> natural_col_widths; // リサイズ高速パス用キャッシュ
    std::pmr::vector<uint32_t> row_flat_offsets; // 各行の線形化テキスト先頭オフセット（ヒットテスト高速化用）
    std::pmr::vector<uint8_t> row_bgs_computed; // 各行のインラインコード背景計算済みフラグ
    // ヒットテスト高速化用の累積オフセット。
    // row_cum_y[r] = エントリ上端からの行 r の上端までの累積高さ。サイズは row_count+1。
    // col_cum_x[c] = base_x からの列 c の左端までの累積幅。サイズは col_count+1。
    std::pmr::vector<float> row_cum_y;
    std::pmr::vector<float> col_cum_x;

    // フラットインデックスへの変換
    constexpr size_t CellIndex(size_t row, size_t col) const noexcept { return row * col_count + col; }

    // セルレイアウトの安全アクセス
    IDWriteTextLayout* GetCellLayout(size_t row, size_t col) const noexcept
    {
        const size_t idx = CellIndex(row, col);
        return (idx < cell_layouts.size()) ? cell_layouts[idx].Get() : nullptr;
    }
};

struct NodeLayoutEntry {
    float y_position = 0.0f;
    float height = 0.0f;
    Microsoft::WRL::ComPtr<IDWriteTextLayout> text_layout;
    bool layout_dirty = true;
    bool effects_applied = false;
    std::pmr::vector<InlineCodeBg> inline_code_bgs;
    std::unique_ptr<TableLayoutData> table_layout; // テーブルのみ確保

    TableLayoutData& ensure_table_layout()
    {
        if (!table_layout) {
            table_layout = std::make_unique<TableLayoutData>();
        }
        return *table_layout;
    }
    bool has_table_layout() const noexcept { return table_layout != nullptr; }

    // table_row に対応する絶対 Y。行情報が無ければ y_position にフォールバックする。
    // row_cum_y は row_count+1 要素（末尾は合計高さ）なので、行として有効なのは [0, row_heights.size()) のみ。
    float GetTableRowY(int table_row) const noexcept
    {
        if (table_row >= 0 && has_table_layout()) {
            const auto row = static_cast<size_t>(table_row);
            if (row < table_layout->row_heights.size() && row < table_layout->row_cum_y.size()) {
                return y_position + table_layout->row_cum_y[row];
            }
        }
        return y_position;
    }

    // table_row の高さ。行情報が無ければエントリ全体の高さにフォールバックする。
    float GetTableRowHeight(int table_row) const noexcept
    {
        if (table_row >= 0 && has_table_layout()) {
            const auto row = static_cast<size_t>(table_row);
            if (row < table_layout->row_heights.size()) {
                return table_layout->row_heights[row];
            }
        }
        return height;
    }
};

struct DiagramEntry {
    Microsoft::WRL::ComPtr<ID2D1Bitmap> bitmap;
    float width = 0.0f;
    float height = 0.0f;
};

class LayoutCache {
public:
    constexpr void Resize(size_t node_count)
    {
        if (entries_.size() != node_count) {
            entries_.resize(node_count);
            diagrams_.resize(node_count);
            effects_generation_++;
        }
    }

    // prefix 部分のキャッシュを保持したままリサイズする。
    // 追加エントリの y_position を旧末尾に配置し、二分探索の単調性を維持する。
    void ResizePreservingPrefix(size_t new_node_count)
    {
        const size_t old_count = entries_.size();
        Resize(new_node_count);
        if (new_node_count > old_count && old_count > 0) {
            const float end_y = entries_[old_count - 1].y_position
                + entries_[old_count - 1].height;
            for (size_t i = old_count; i < new_node_count; i++) {
                entries_[i].y_position = end_y;
            }
        }
    }

    // 既存のエントリをすべてクリアし、デフォルト値でリサイズする。
    // shrink=true (デフォルト): ファイル切り替え時に古い容量を解放する。
    // shrink=false: リロード時に容量を保持し、同サイズファイルの再確保を回避する。
    void Reset(size_t node_count, bool shrink = true)
    {
        entries_.clear();
        if (shrink) {
            entries_.shrink_to_fit();
        }
        entries_.resize(node_count);
        diagrams_.clear();
        if (shrink) {
            diagrams_.shrink_to_fit();
        }
        diagrams_.resize(node_count);
        effects_generation_++;
    }

    constexpr size_t size() const noexcept { return entries_.size(); }

    using const_iterator = std::pmr::vector<NodeLayoutEntry>::const_iterator;

    constexpr const_iterator cbegin() const noexcept { return entries_.begin(); }
    constexpr const_iterator cend() const noexcept { return entries_.end(); }

    constexpr NodeLayoutEntry& operator[](size_t i) noexcept
    {
        assert(i < entries_.size());
        return entries_[i];
    }
    constexpr const NodeLayoutEntry& operator[](size_t i) const noexcept
    {
        assert(i < entries_.size());
        return entries_[i];
    }

    constexpr DiagramEntry& GetDiagram(size_t i) noexcept
    {
        assert(i < diagrams_.size());
        return diagrams_[i];
    }
    constexpr const DiagramEntry& GetDiagram(size_t i) const noexcept
    {
        assert(i < diagrams_.size());
        return diagrams_[i];
    }

    // すべてのテキストレイアウトとエフェクトを無効化する（テーマ/ズーム変更時）。
    // ダイアグラム/Mermaid キャッシュの処理は呼び出し側で別途行うこと。
    void InvalidateAllLayouts() noexcept
    {
        for (auto& e : entries_) {
            e.text_layout.Reset();
            e.effects_applied = false;
            e.inline_code_bgs.clear();
            if (e.table_layout) {
                e.table_layout->cell_layouts.clear();
                e.table_layout->cell_inline_code_bgs.clear();
                e.table_layout->row_bgs_computed.clear();
                e.table_layout->natural_col_widths.clear();
                e.table_layout->col_count = 0;
            }
        }
        effects_generation_++;
    }

    // すべてのテキストレイアウトとエフェクトを無効化し、Mermaid図のビットマップもリセットする。
    // ダークモード切替時に使用。
    void InvalidateAllWithDiagrams(const std::pmr::vector<Node>& nodes) noexcept
    {
        InvalidateAllLayouts(); // effects_generation_ は InvalidateAllLayouts 内で更新済み
        InvalidateDiagramBitmaps(nodes);
    }

    // ダイアグラム系ノードのビットマップを無効化する。
    // ダークモード切替時に text_layout とエフェクトを保持したまま図だけ再描画したい場合に使う。
    void InvalidateDiagramBitmaps(const std::pmr::vector<Node>& nodes) noexcept
    {
        const auto count = std::min(nodes.size(), diagrams_.size());
        for (const auto& [idx, node] : nodes | std::views::take(count) | std::views::enumerate) {
            if (IsDiagramLanguage(node.code_language)) {
                diagrams_[static_cast<size_t>(idx)].bitmap.Reset();
            }
        }
    }

    // すべてのエントリをダーティとしてマークし、レイアウトをリセットする（DPI 変更時）。
    void MarkAllDirty() noexcept
    {
        for (auto& e : entries_) {
            e.layout_dirty = true;
            e.text_layout.Reset();
            if (e.table_layout) {
                e.table_layout->cell_layouts.clear();
                e.table_layout->row_bgs_computed.clear();
                e.table_layout->natural_col_widths.clear();
                e.table_layout->col_count = 0;
            }
        }
        effects_generation_++;
    }

    // エフェクト世代カウンタ。レイアウト変更時にインクリメントされる。
    // Renderer が ApplyVisibleEffects のスキップ判定に使用する。
    constexpr uint32_t GetEffectsGeneration() const noexcept { return effects_generation_; }
    void IncrementEffectsGeneration() noexcept { effects_generation_++; }

private:
    std::pmr::vector<NodeLayoutEntry> entries_;
    std::pmr::vector<DiagramEntry> diagrams_;
    uint32_t effects_generation_ = 0;
};

// 最後のノードのレイアウト位置からコンテンツ全体の高さを計算する。
// node_count が 0 の場合は 0 を返し、size() - 1 の符号なし整数アンダーフローを回避する。
constexpr float ComputeTotalContentHeight(const LayoutCache& cache, size_t node_count, float margin_top) noexcept
{
    if (node_count == 0) {
        return 0.0f;
    }
    const size_t last = node_count - 1;
    return cache[last].y_position + cache[last].height + margin_top;
}

// ノードの Y 範囲 [y, y+h] が [range_top, range_bottom] と重ならない場合 true を返す。
// 端が接している場合（y+h == range_top 等）は重なり扱い（false）。
constexpr bool IsOffscreen(float y, float h, float range_top, float range_bottom) noexcept
{
    return y + h < range_top || y > range_bottom;
}

// 下端が viewport_top 以上の最初のノードを二分探索で見つける。
// 最初の可視候補ノードのインデックスを返す。該当なしの場合は node_count を返す。
constexpr int FindFirstVisibleNodeIndex(const LayoutCache& cache, size_t node_count, float viewport_top) noexcept
{
    const auto first = cache.cbegin();
    const auto last = first + static_cast<ptrdiff_t>(node_count);
    const auto it = std::ranges::partition_point(first, last, [viewport_top](const NodeLayoutEntry& e) noexcept {
        return e.y_position + e.height <= viewport_top;
    });
    return static_cast<int>(it - first);
}
