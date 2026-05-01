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
#include <span>


// インラインコードの背景矩形（レイアウト原点からの相対座標、パディング適用済み）
using InlineCodeBg = D2D1_RECT_F;

// テーブル内インラインコード背景の 1 エントリ。
// flat 化することで、bg を持たないセル分の空 vector ヘッダ (24B/個) を排除する。
// 書き込みは row 昇順 → col 昇順、描画も同順なので cell_index は概ね昇順で並ぶ。
struct CellInlineCodeBg {
    uint32_t cell_index; // row * col_count + col
    InlineCodeBg rect;
};

// テーブル専用のレイアウトデータ（テーブルノードのみ確保してメモリを節約）
struct TableLayoutData {
    std::pmr::vector<Microsoft::WRL::ComPtr<IDWriteTextLayout>> cell_layouts; // フラット配列 [行 * col_count + 列]
    std::pmr::vector<CellInlineCodeBg> cell_inline_code_bgs;                  // セル別 bg を線形に格納（cell_index 昇順）
    size_t col_count = 0;                                                     // cell_layoutsのストライド
    std::pmr::vector<float> col_widths;
    std::pmr::vector<float> row_heights;
    std::pmr::vector<float> natural_col_widths;  // リサイズ高速パス用キャッシュ
    std::pmr::vector<float> cell_heights;        // 各セルに最後に適用した幅での計測高さ
    std::pmr::vector<float> cell_applied_widths; // 各セルに最後に適用した max_width（変更判定用）
    std::pmr::vector<uint32_t> row_flat_offsets; // 各行の線形化テキスト先頭オフセット（ヒットテスト高速化用）
    std::pmr::vector<uint8_t> row_bgs_computed;  // 各行のインラインコード背景計算済みフラグ
    // ヒットテスト高速化用の累積オフセット。
    // row_cum_y[r] = エントリ上端からの行 r の上端までの累積高さ。サイズは row_count+1。
    // col_cum_x[c] = base_x からの列 c の左端までの累積幅。サイズは col_count+1。
    std::pmr::vector<float> row_cum_y;
    std::pmr::vector<float> col_cum_x;
    // FinalizeTableLayout に最後に渡された max_width。CELL_WIDTH_EPSILON 以内の
    // 差分なら全体の再計算を完全スキップできる。負値 = 未設定。
    float last_applied_max_width = -1.0f;
    // 列幅 + パディング + 罫線の合計。FinalizeTableLayout で確定後不変なので
    // GenTable から毎フレーム fold_left せずキャッシュを参照する。
    float cached_table_width = 0.0f;

    // フラットインデックスへの変換
    constexpr size_t CellIndex(size_t row, size_t col) const noexcept
    {
        return row * col_count + col;
    }

    // セルレイアウトの安全アクセス
    IDWriteTextLayout* GetCellLayout(size_t row, size_t col) const noexcept
    {
        const size_t idx = CellIndex(row, col);
        return (idx < cell_layouts.size()) ? cell_layouts[idx].Get() : nullptr;
    }

    // 行内 [col_from, col_to) のセル幅とタブ区切りだけ flat_offset を進める。
    // 線形化規約は dwrite_measurer.cpp の row_flat_offsets 構築と一致する。
    static void AdvanceFlatOffsetInRow(const TableRow& row,
                                       size_t col_from, size_t col_to, uint32_t& flat_offset) noexcept
    {
        const auto col_count_actual = row.cells.size();
        const size_t end = std::min(col_to, col_count_actual);
        for (size_t c = col_from; c < end; c++) {
            flat_offset += static_cast<uint32_t>(row.cells[c].text.size());
            if (c + 1 < col_count_actual) {
                flat_offset++;
            }
        }
    }

    // (row_idx, col) セルの線形化テキスト先頭オフセット。
    // dwrite_measurer の MeasureTable がレイアウト確定時に row_flat_offsets を必ず埋めるため、
    // ここでの存在は前提扱い。
    uint32_t CellFlatOffset(const std::pmr::vector<TableRow>& rows,
                            size_t row_idx, size_t col) const noexcept
    {
        assert(row_idx < row_flat_offsets.size());
        uint32_t offset = row_flat_offsets[row_idx];
        AdvanceFlatOffsetInRow(rows[row_idx], 0, col, offset);
        return offset;
    }
};

// 検索ハイライト矩形のフレーム間キャッシュ。
// match i (ノード内順) の矩形は rects[rect_ends[i-1] ... rect_ends[i]) (i=0 なら 0 始まり)。
// gen が SearchState の generation と一致する間は HitTestTextRange 再計算をスキップする。
struct SearchHlCache {
    std::pmr::vector<D2D1_RECT_F> rects;
    std::pmr::vector<uint32_t> rect_ends;
    uint32_t gen = 0;
};

// 選択ハイライト矩形のフレーム間キャッシュ。本文ノード（テーブル外）でのみ使用。
// (layout, start, length) が一致する間は HitTestTextRange 再計算をスキップする。
// 新規構築時の layout_ptr=nullptr は確実にミス判定になるため、validity フラグは持たない。
// text_layout 失効時は invalidate_selection_hl_cache() で unique_ptr ごと破棄する契約。
struct SelectionHlCache {
    std::pmr::vector<D2D1_RECT_F> rects;
    IDWriteTextLayout* layout_ptr = nullptr;
    uint32_t start = 0;
    uint32_t length = 0;
};

struct NodeLayoutEntry {
    float y_position = 0.0f;
    float height = 0.0f;
    // GetLineMetrics(&lm, 1, &lc) の結果をキャッシュ。0 なら未確定 (フォールバック計算する)。
    // MeasureNode で text_layout 確定時に同時に求める。
    float first_line_height = 0.0f;
    Microsoft::WRL::ComPtr<IDWriteTextLayout> text_layout;
    bool layout_dirty = true;
    bool effects_applied = false;
    // インラインコード持ちノードでのみ確保される。空の vector ヘッダ (24B/個) を全ノード分背負わない。
    std::unique_ptr<std::pmr::vector<InlineCodeBg>> inline_code_bgs;
    std::unique_ptr<TableLayoutData> table_layout; // テーブルのみ確保

    // 検索ヒットがあるノードでのみ確保される。描画中に書き換えるため mutable。
    mutable std::unique_ptr<SearchHlCache> search_hl_cache;
    // 選択中のノードでのみ確保される。描画中に書き換えるため mutable。
    mutable std::unique_ptr<SelectionHlCache> selection_hl_cache;

    SearchHlCache& ensure_search_hl_cache() const
    {
        if (!search_hl_cache) {
            search_hl_cache = std::make_unique<SearchHlCache>();
        }
        return *search_hl_cache;
    }

    void invalidate_search_hl_cache() const noexcept
    {
        search_hl_cache.reset();
    }

    SelectionHlCache& ensure_selection_hl_cache() const
    {
        if (!selection_hl_cache) {
            selection_hl_cache = std::make_unique<SelectionHlCache>();
        }
        return *selection_hl_cache;
    }

    void invalidate_selection_hl_cache() const noexcept
    {
        selection_hl_cache.reset();
    }

    // text_layout の wrap や format 属性が変わった際に、行折り返し由来のキャッシュ
    // (検索ハイライト矩形 / 選択ハイライト矩形) を一括で落とす。
    void invalidate_per_frame_hl_caches() const noexcept
    {
        invalidate_search_hl_cache();
        invalidate_selection_hl_cache();
    }

    TableLayoutData& ensure_table_layout()
    {
        if (!table_layout) {
            table_layout = std::make_unique<TableLayoutData>();
        }
        return *table_layout;
    }
    constexpr bool has_table_layout() const noexcept
    {
        return table_layout != nullptr;
    }

    std::pmr::vector<InlineCodeBg>& ensure_inline_code_bgs()
    {
        if (!inline_code_bgs) {
            inline_code_bgs = std::make_unique<std::pmr::vector<InlineCodeBg>>();
        }
        return *inline_code_bgs;
    }
    void clear_inline_code_bgs() noexcept
    {
        inline_code_bgs.reset();
    }
    std::span<const InlineCodeBg> view_inline_code_bgs() const noexcept
    {
        return SpanOrEmpty(inline_code_bgs);
    }

    // マッチの絶対 Y とその行の高さを返す。text_layout（テーブルはセル layout）が
    // あれば text_offset に対応する行 Y を精密に計算し、無ければ
    // ブロック先頭/テーブル行先頭の座標にフォールバックする。
    // Why: 長い段落内の複数マッチで同じブロック先頭 Y に丸まると「次へ」でスクロールしない。
    // row_cum_y は row_count+1 要素（末尾は合計高さ）なので、行として有効なのは [0, row_heights.size()) のみ。
    std::pair<float, float> GetMatchYRange(int table_row, int table_col, uint32_t text_offset) const noexcept
    {
        IDWriteTextLayout* layout = nullptr;
        float base_y = y_position;
        float fallback_h = height;

        if (table_row >= 0 && has_table_layout()) {
            const auto row = static_cast<size_t>(table_row);
            if (row < table_layout->row_heights.size() && row < table_layout->row_cum_y.size()) {
                base_y = y_position + table_layout->row_cum_y[row];
                fallback_h = table_layout->row_heights[row];
            }
            if (table_col >= 0) {
                layout = table_layout->GetCellLayout(row, static_cast<size_t>(table_col));
            }
        }
        else {
            layout = text_layout.Get();
        }

        if (layout != nullptr) {
            FLOAT px = 0.0f, py = 0.0f;
            DWRITE_HIT_TEST_METRICS htm{};
            if (SUCCEEDED(layout->HitTestTextPosition(text_offset, FALSE, &px, &py, &htm))) {
                const float line_h = (htm.height > 0.0f) ? htm.height : fallback_h;
                return { base_y + py, line_h };
            }
        }
        return { base_y, fallback_h };
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
    constexpr void ResizePreservingPrefix(size_t new_node_count)
    {
        const size_t old_count = entries_.size();
        Resize(new_node_count);
        if (new_node_count > old_count && old_count > 0) {
            const float end_y = entries_[old_count - 1].y_position + entries_[old_count - 1].height;
            for (size_t i = old_count; i < new_node_count; i++) {
                entries_[i].y_position = end_y;
            }
        }
    }

    constexpr void Reset(size_t node_count, bool shrink = true)
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

    constexpr size_t size() const noexcept
    {
        return entries_.size();
    }

    using const_iterator = std::pmr::vector<NodeLayoutEntry>::const_iterator;

    constexpr const_iterator cbegin() const noexcept
    {
        return entries_.begin();
    }
    constexpr const_iterator cend() const noexcept
    {
        return entries_.end();
    }

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

    // 全エントリの selection_hl_cache を破棄する。
    // Why: SelectionHlCache は描画時に lazy 確保され、選択範囲外に出たノードでは
    // 自動破棄経路が無いため、長時間利用で過去に選択したノード分の unique_ptr/rects
    // が居残ってメモリが漸増する。CommandGenerator 側で「選択範囲が縮小/解除された
    // フレーム」を検出して、対象ノードの cache を巻き戻すのに使う。
    void ClearAllSelectionHlCaches() const noexcept
    {
        for (const auto& e : entries_) {
            e.invalidate_selection_hl_cache();
        }
    }

    // すべてのテキストレイアウトとエフェクトを無効化する（テーマ/ズーム変更時）。
    // ダイアグラム/Mermaid キャッシュの処理は呼び出し側で別途行うこと。
    void InvalidateAllLayouts() noexcept
    {
        for (auto& e : entries_) {
            e.text_layout.Reset();
            e.effects_applied = false;
            e.first_line_height = 0.0f;
            e.clear_inline_code_bgs();
            e.invalidate_per_frame_hl_caches();
            if (e.table_layout) {
                ResetTableLayoutGeometry(*e.table_layout);
                e.table_layout->cell_inline_code_bgs.clear();
            }
        }
        effects_generation_++;
    }

    // フォント幾何が変わるテーマ変更（ズーム等）用。レイアウトと bitmap の両方を破棄する。
    // 色のみの変更なら InvalidateEffectsAndDiagramBitmaps の方がレイアウト維持で軽い。
    void InvalidateAllWithDiagrams(const std::pmr::vector<Node>& nodes) noexcept
    {
        InvalidateAllLayouts();
        InvalidateDiagramBitmaps(nodes);
    }

    // 色のみが変わるテーマ変更（ライト/ダーク切替）用。
    // 文字幾何 (IDWriteTextLayout / table cell layouts) は維持し、ApplyEffects を再走らせるため
    // effects_applied フラグだけ落とす。Mermaid bitmap はテーマ色を持つので破棄する。
    void InvalidateEffectsAndDiagramBitmaps(const std::pmr::vector<Node>& nodes) noexcept
    {
        for (auto& e : entries_) {
            e.effects_applied = false;
            e.clear_inline_code_bgs();
            if (e.table_layout) {
                e.table_layout->cell_inline_code_bgs.clear();
                e.table_layout->row_bgs_computed.clear();
            }
        }
        effects_generation_++;
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

    // 可視範囲外（[0, first_keep) と [last_keep, size())）のノードについて
    // text_layout / table_layout / inline_code_bgs / search_hl_cache を破棄する。
    // y_position と height は維持され、layout_dirty=true で再生成可能な状態にする。
    // Why: IDWriteTextLayout は内部で glyph buffer / bidi 解析を保持し
    // 1K 文字あたり 2-5KB。可視外も保持し続けると巨大ドキュメントで数十 MB
    // のメモリを COM 側に常駐させてしまう。EVICT_BUFFER_SCREENS 分の安全マージン
    // を取った上で範囲外を解放する。呼び出し側（ResourceManager）は再描画時の
    // 再生成コストを踏まえてタイマー駆動で間引く。
    void EvictTextLayouts(size_t first_keep, size_t last_keep) noexcept
    {
        const size_t n = entries_.size();
        const size_t fk = std::min(first_keep, n);
        const size_t lk = std::min(last_keep, n);
        for (size_t i = 0; i < fk; ++i) {
            EvictEntryLayout(entries_[i]);
        }
        for (size_t i = lk; i < n; ++i) {
            EvictEntryLayout(entries_[i]);
        }
    }

    // すべてのエントリをダーティとしてマークし、レイアウトをリセットする（DPI 変更時）。
    void MarkAllDirty() noexcept
    {
        for (auto& e : entries_) {
            e.layout_dirty = true;
            e.text_layout.Reset();
            e.first_line_height = 0.0f;
            e.invalidate_per_frame_hl_caches();
            if (e.table_layout) {
                ResetTableLayoutGeometry(*e.table_layout);
            }
        }
        effects_generation_++;
    }

    // エフェクト世代カウンタ。レイアウト変更時にインクリメントされる。
    // Renderer が ApplyVisibleEffects のスキップ判定に使用する。
    constexpr uint32_t GetEffectsGeneration() const noexcept
    {
        return effects_generation_;
    }
    constexpr void IncrementEffectsGeneration() noexcept
    {
        effects_generation_++;
    }

private:
    // 列幅 / 行高さ / セルメトリクス系の派生キャッシュを既定値に戻す。
    // cell_inline_code_bgs は呼び出し側で必要に応じて別途クリアする
    // (DPI 変更パスは bg を作り直すまで維持する設計)。
    static void ResetTableLayoutGeometry(TableLayoutData& tl) noexcept
    {
        tl.cell_layouts.clear();
        tl.row_bgs_computed.clear();
        tl.natural_col_widths.clear();
        tl.cell_heights.clear();
        tl.cell_applied_widths.clear();
        tl.col_count = 0;
        tl.last_applied_max_width = -1.0f;
        tl.cached_table_width = 0.0f;
    }

    static void EvictEntryLayout(NodeLayoutEntry& e) noexcept
    {
        if (!e.text_layout && !e.table_layout) {
            return;
        }
        e.text_layout.Reset();
        e.effects_applied = false;
        e.first_line_height = 0.0f;
        e.clear_inline_code_bgs();
        e.table_layout.reset();
        e.layout_dirty = true;
        e.invalidate_per_frame_hl_caches();
    }

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
// 最初の可視候補ノードのインデックスを返す。該当なしの場合は effective node count を返す。
// 過渡状態で node_count > cache.size() でも UB にならないよう内部でクランプする。
constexpr int FindFirstVisibleNodeIndex(const LayoutCache& cache, size_t node_count, float viewport_top) noexcept
{
    const size_t effective = std::min(node_count, cache.size());
    const auto first = cache.cbegin();
    const auto last = first + static_cast<ptrdiff_t>(effective);
    const auto it = std::ranges::partition_point(first, last, [viewport_top](const NodeLayoutEntry& e) noexcept {
        return e.y_position + e.height <= viewport_top;
    });
    return static_cast<int>(it - first);
}

// (node, offset) → 絶対スクロール位置。
// 負の node や空キャッシュは 0 を返し、末尾を超える node は最後の要素へクランプする
constexpr float NodeOffsetToScrollY(const LayoutCache& cache, int node, float offset) noexcept
{
    if (cache.size() == 0 || node < 0) {
        return 0.0f;
    }
    const int clamped = std::min(node, static_cast<int>(cache.size()) - 1);
    return std::max(0.0f, cache[clamped].y_position + offset);
}
