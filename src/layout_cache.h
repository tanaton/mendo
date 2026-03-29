#pragma once
#include "types.h"
#include <vector>
#include <memory_resource>
#include <wrl/client.h>
#include <d2d1.h>
#include <dwrite.h>
#include <cassert>

using Microsoft::WRL::ComPtr;

struct InlineCodeBg {
    float left, top, width, height;
};

struct NodeLayoutEntry {
    float y_position = 0.0f;
    float height = 0.0f;
    ComPtr<IDWriteTextLayout> text_layout;
    bool layout_dirty = true;
    bool effects_applied = false;
    std::pmr::vector<InlineCodeBg> inline_code_bgs;

    // テーブルレイアウトデータ
    std::pmr::vector<std::pmr::vector<ComPtr<IDWriteTextLayout>>> cell_layouts; // [行][列]
    std::pmr::vector<std::pmr::vector<std::pmr::vector<InlineCodeBg>>> cell_inline_code_bgs; // [行][列][]
    std::pmr::vector<float> col_widths;
    std::pmr::vector<float> row_heights;
    std::pmr::vector<float> natural_col_widths; // リサイズ高速パス用キャッシュ
};

struct DiagramEntry {
    ComPtr<ID2D1Bitmap> bitmap;
    float width = 0.0f;
    float height = 0.0f;
};

class LayoutCache {
public:
    constexpr void Resize(size_t node_count)
    {
        entries_.resize(node_count);
        diagrams_.resize(node_count);
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
    }

    constexpr size_t size() const noexcept { return entries_.size(); }

    constexpr NodeLayoutEntry& operator[](size_t i) noexcept { assert(i < entries_.size()); return entries_[i]; }
    constexpr const NodeLayoutEntry& operator[](size_t i) const noexcept { assert(i < entries_.size()); return entries_[i]; }

    constexpr DiagramEntry& GetDiagram(size_t i) noexcept { assert(i < diagrams_.size()); return diagrams_[i]; }
    constexpr const DiagramEntry& GetDiagram(size_t i) const noexcept { assert(i < diagrams_.size()); return diagrams_[i]; }

    // すべてのテキストレイアウトとエフェクトを無効化する（テーマ/ズーム変更時）。
    // ダイアグラム/Mermaid キャッシュの処理は呼び出し側で別途行うこと。
    void InvalidateAllLayouts()
    {
        for (auto& e : entries_) {
            e.text_layout.Reset();
            e.effects_applied = false;
            e.inline_code_bgs.clear();
            e.cell_inline_code_bgs.clear();
        }
    }

    // すべてのテキストレイアウトとエフェクトを無効化し、Mermaid図のビットマップもリセットする。
    // ダークモード切替時に使用。
    void InvalidateAllWithDiagrams(const std::pmr::vector<Node>& nodes)
    {
        InvalidateAllLayouts();
        for (size_t i = 0; i < nodes.size() && i < diagrams_.size(); ++i) {
            if (nodes[i].code_language == SyntaxLanguage::Mermaid) {
                diagrams_[i].bitmap.Reset();
            }
        }
    }

    // すべてのエントリをダーティとしてマークし、レイアウトをリセットする（DPI 変更時）。
    void MarkAllDirty()
    {
        for (auto& e : entries_) {
            e.layout_dirty = true;
            e.text_layout.Reset();
        }
    }

private:
    std::pmr::vector<NodeLayoutEntry> entries_;
    std::pmr::vector<DiagramEntry> diagrams_;
};

// 最後のノードのレイアウト位置からコンテンツ全体の高さを計算する。
// node_count が 0 の場合は 0 を返し、size() - 1 の符号なし整数アンダーフローを回避する。
constexpr float ComputeTotalContentHeight(const LayoutCache& cache, size_t node_count, float margin_top) noexcept
{
    if (node_count == 0) {
        return 0.0f;
    }
    size_t last = node_count - 1;
    return cache[last].y_position + cache[last].height + margin_top;
}

// 下端が viewport_top 以上の最初のノードを二分探索で見つける。
// 最初の可視候補ノードのインデックスを返す。該当なしの場合は node_count を返す。
constexpr int FindFirstVisibleNodeIndex(const LayoutCache& cache, size_t node_count, float viewport_top) noexcept
{
    int lo = 0, hi = static_cast<int>(node_count);
    while (lo < hi) {
        int mid = (lo + hi) / 2;
        if (cache[mid].y_position + cache[mid].height <= viewport_top) {
            lo = mid + 1;
        }
        else {
            hi = mid;
        }
    }
    return lo;
}
