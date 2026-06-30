#pragma once
#include "document_types.h"
#include "fenwick.h"
#include "pmr_unique_ptr.h"
#include <vector>
#include <memory>
#include <memory_resource>
#include <wrl/client.h>
#include <d2d1.h>
#include <dwrite.h>
#include <algorithm>
#include <ranges>
#include <span>
#include <utility>

struct Theme;


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
    // 圧縮を行わなかった場合の自然な総幅。横スクロールのクランプ計算用に保持する。
    // cached_table_width と一致することもあるが、圧縮分岐を通った場合は乖離する。
    float natural_total_width = 0.0f;
    // 立っている間は MeasureTable の超高速パスをバイパスし RestoreNullCellLayouts を走らせる。
    // FinalizeTableLayout 完了でクリア。
    bool cells_partially_evicted = false;
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

    // [local_top, local_bottom] に重なる行帯 [r_begin, r_end) を二分探索で返す。
    // 座標はいずれもエントリ上端からのローカル系。row_cum_y が空なら [0, 0)。
    std::pair<size_t, size_t> VisibleRowRange(float local_top, float local_bottom) const noexcept;

    // local_y がヒットする行インデックスを返す。ヒットなしは -1。
    // 座標はエントリ上端からのローカル系。row_cum_y が空でもヒットなし扱い。
    int RowIndexAt(float local_y) const noexcept;
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

namespace mendo::layout::detail {

// pmr_unique_ptr の lazy 初期化ヘルパ。NodeLayoutEntry のキャッシュ群で共有する。
template <typename T>
constexpr T& EnsurePmrUnique(mendo::pmr_unique_ptr<T>& p)
{
    if (!p) {
        p = mendo::MakePmrUnique<T>();
    }
    return *p;
}

} // namespace mendo::layout::detail

struct NodeLayoutEntry {
    // テキスト上端 Y の denormalized cache。値としては cache.GetBlockTop(i, margin_top) + GetSpacingAbove(node)
    // と等価で、TextTopOf でも導出可能だが、その経路は Fenwick PrefixSum で O(log N) かかる。
    // hit-test の partition_point 述語 / visible-node loop / scroll bound 計算など per-frame hot path で
    // 直接 O(1) 参照したいため field として保持する (TextTopOf は navigation/reload 等の cold path 専用)。
    // WRITE 経路 (RecomputeYPositions / ComputeLayout / EstimateNodeHeights / ResizePreservingPrefix) で
    // Fenwick block_heights_ と lockstep 同期される。MeasureNode は触らない (= 中間状態は古い値のまま)。
    float text_top = 0.0f;
    float height = 0.0f;
    // GetLineMetrics(&lm, 1, &lc) の結果をキャッシュ。0 なら未確定 (フォールバック計算する)。
    // MeasureNode で text_layout 確定時に同時に求める。
    float first_line_height = 0.0f;
    // 直近 MeasureNode の幅と実測 height。同じ幅へ戻った際に EstimateNodeHeight の上書きを避ける。
    // 「未計測」のセンチネルは kUnmeasuredWidth (= 負値)。MeasureNode 入力 (node_width) は
    // 常に正なので負値は初期化/invalidate 経路でしか出現しない。per-node で密に並ぶ hot field の
    // ため optional<float> 化はサイズ膨張を招き避けている (is_measured() で判定統一)。
    static constexpr float kUnmeasuredWidth = -1.0f;
    float cached_width = kUnmeasuredWidth;
    float cached_height = 0.0f;
    // CodeBlock の最長行幅 (NO_WRAP で計測した自然幅)。横スクロールバーのスケール計算と
    // クランプに使う。CodeBlock 以外では 0 のまま。
    float natural_code_width = 0.0f;
    constexpr bool is_measured() const noexcept
    {
        return cached_width > 0.0f;
    }
    Microsoft::WRL::ComPtr<IDWriteTextLayout> text_layout;
    bool layout_dirty = true;
    bool effects_applied = false;
    // インラインコード持ちノードでのみ確保される。空の vector ヘッダ (24B/個) を全ノード分背負わない。
    mendo::pmr_unique_ptr<std::pmr::vector<InlineCodeBg>> inline_code_bgs;
    mendo::pmr_unique_ptr<TableLayoutData> table_layout; // テーブルのみ確保

    // 検索ヒットがあるノードでのみ確保される。描画中に書き換えるため mutable。
    mutable mendo::pmr_unique_ptr<SearchHlCache> search_hl_cache;
    // 選択中のノードでのみ確保される。描画中に書き換えるため mutable。
    mutable mendo::pmr_unique_ptr<SelectionHlCache> selection_hl_cache;

    constexpr SearchHlCache& ensure_search_hl_cache() const
    {
        return mendo::layout::detail::EnsurePmrUnique(search_hl_cache);
    }

    constexpr void invalidate_search_hl_cache() const noexcept
    {
        search_hl_cache.reset();
    }

    constexpr SelectionHlCache& ensure_selection_hl_cache() const
    {
        return mendo::layout::detail::EnsurePmrUnique(selection_hl_cache);
    }

    constexpr void invalidate_selection_hl_cache() const noexcept
    {
        selection_hl_cache.reset();
    }

    // text_layout の wrap や format 属性が変わった際に、行折り返し由来のキャッシュ
    // (検索ハイライト矩形 / 選択ハイライト矩形) を一括で落とす。
    constexpr void invalidate_per_frame_hl_caches() const noexcept
    {
        invalidate_search_hl_cache();
        invalidate_selection_hl_cache();
    }

    constexpr TableLayoutData& ensure_table_layout()
    {
        return mendo::layout::detail::EnsurePmrUnique(table_layout);
    }
    constexpr bool has_table_layout() const noexcept
    {
        return table_layout != nullptr;
    }

    constexpr std::pmr::vector<InlineCodeBg>& ensure_inline_code_bgs()
    {
        return mendo::layout::detail::EnsurePmrUnique(inline_code_bgs);
    }

    constexpr void clear_inline_code_bgs() noexcept
    {
        inline_code_bgs.reset();
    }

    constexpr std::span<const InlineCodeBg> view_inline_code_bgs() const noexcept
    {
        return SpanOrEmpty(inline_code_bgs);
    }

    // マッチの絶対 Y とその行の高さを返す。text_layout（テーブルはセル layout）が
    // あれば text_offset_w (UTF-16 code unit) に対応する行 Y を精密に計算し、無ければ
    // ブロック先頭/テーブル行先頭の座標にフォールバックする。
    // Why: 長い段落内の複数マッチで同じブロック先頭 Y に丸まると「次へ」でスクロールしない。
    // text_offset_w は IDWriteTextLayout::HitTestTextPosition の引数なので UTF-16 単位を渡すこと
    // (UTF-8 byte を渡すと非 ASCII を含む段落で行 Y がずれる)。
    std::pair<float, float> GetMatchYRange(int table_row, int table_col, uint32_t text_offset_w, float entry_text_top) const noexcept;
};

struct DiagramEntry {
    Microsoft::WRL::ComPtr<ID2D1Bitmap> bitmap;
    float width = 0.0f;
    float height = 0.0f;
    // クリップボードコピー用の元 PNG。bitmap と同時に設定/破棄されるため、
    // コピーボタンの表示条件 (bitmap 有無) とデータの有無が常に一致する。
    std::shared_ptr<const std::pmr::vector<uint8_t>> png;
};

class LayoutCache {
public:
    void Resize(size_t node_count);

    // prefix 部分のキャッシュを保持したままリサイズする。
    // 追加エントリの text_top を旧末尾に配置し、二分探索の単調性を維持する。
    void ResizePreservingPrefix(size_t new_node_count);

    void Reset(size_t node_count, bool shrink = true);

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
        return entries_[i];
    }
    constexpr const NodeLayoutEntry& operator[](size_t i) const noexcept
    {
        return entries_[i];
    }

    constexpr DiagramEntry& GetDiagram(size_t i) noexcept
    {
        return diagrams_[i];
    }
    constexpr const DiagramEntry& GetDiagram(size_t i) const noexcept
    {
        return diagrams_[i];
    }

    // すべてのテキストレイアウトとエフェクトを無効化する（テーマ/ズーム変更時）。
    // ダイアグラム/Mermaid キャッシュの処理は呼び出し側で別途行うこと。
    void InvalidateAllLayouts() noexcept;

    // フォント幾何が変わるテーマ変更（ズーム等）用。レイアウトと bitmap の両方を破棄する。
    // 色のみの変更なら InvalidateEffectsAndDiagramBitmaps の方がレイアウト維持で軽い。
    void InvalidateAllWithDiagrams(const std::pmr::vector<Node>& nodes) noexcept;

    // 色のみが変わるテーマ変更（ライト/ダーク切替）用。
    // 文字幾何 (IDWriteTextLayout / table cell layouts) は維持し、ApplyEffects を再走らせるため
    // effects_applied フラグだけ落とす。Mermaid bitmap はテーマ色を持つので破棄する。
    void InvalidateEffectsAndDiagramBitmaps(const std::pmr::vector<Node>& nodes) noexcept;

    // デバイスロスト時用。旧デバイス上のビットマップは新 RT で描画できず、
    // 残すと EndDraw が D2DERR_WRONG_RESOURCE_DOMAIN で失敗し続ける。
    void InvalidateAllDiagramBitmaps() noexcept;

    // ダイアグラム系ノードのビットマップを無効化する。
    // ダークモード切替時に text_layout とエフェクトを保持したまま図だけ再描画したい場合に使う。
    void InvalidateDiagramBitmaps(const std::pmr::vector<Node>& nodes) noexcept;

    // 可視範囲外（[0, first_keep) と [last_keep, size())）のノードについて
    // text_layout / table_layout / inline_code_bgs / search_hl_cache を破棄する。
    // text_top と height は維持され、layout_dirty=true で再生成可能な状態にする。
    // Why: IDWriteTextLayout は内部で glyph buffer / bidi 解析を保持し
    // 1K 文字あたり 2-5KB。可視外も保持し続けると巨大ドキュメントで数十 MB
    // のメモリを COM 側に常駐させてしまう。EVICT_BUFFER_SCREENS 分の安全マージン
    // を取った上で範囲外を解放する。呼び出し側（ResourceManager）は再描画時の
    // 再生成コストを踏まえてタイマー駆動で間引く。
    // 破棄範囲は [0, first_keep_inclusive) と [last_keep_exclusive, size)。
    void EvictTextLayouts(size_t first_keep_inclusive, size_t last_keep_exclusive) noexcept;

    // 全 text_layout を破棄する操作の後に呼ぶ。差分エビクトが「破棄済み」と誤認して
    // 再生成済みの画面外レイアウトを取り逃がさないよう、追跡境界を初期化する。
    void ResetEvictionTracking() noexcept;

    // 可視範囲をまたぐテーブルの可視外行で cell_layouts を Reset する。
    // 再表示時は MeasureTable の lazy 復元経路で CreateTextLayout を再発行する。
    // viewport は文書グローバル座標で渡す (entry ローカル座標ではない)。
    // table_indices は Document::GetTableNodeIndices()。全エントリ走査だと 100MB 級
    // 文書でスクロール休止のたびに ~100万エントリを読むため、index リストで絞る。
    void EvictInvisibleTableRows(std::span<const size_t> table_indices, float viewport_top, float viewport_bottom, float buffer_screens_height) noexcept;

    // フォント・テーマ・ズーム変更時の全リセット。DPI 単独変更には NotifyDpiChanged を使う。
    void MarkAllDirty() noexcept;

    // DPI 変更時の最小リセット。IDWriteTextLayout は DIP 単位なので不変、effects_generation のみ進める。
    void NotifyDpiChanged() noexcept;

    // ノード数の変化または全件 invalidate でのみ進める (per-node の dirty では進めない)。
    // Renderer が ApplyVisibleEffects のスキップ判定に使う。
    constexpr uint32_t GetEffectsGeneration() const noexcept
    {
        return effects_generation_;
    }
    constexpr void IncrementEffectsGeneration() noexcept
    {
        effects_generation_++;
    }

    // ノード i の block height (= spacing_above + height + spacing_below) を Fenwick に反映する。
    // RecomputeYPositions が各ノードの新しいブロック高さを書き込む際に呼ぶ。
    void SetBlockHeight(size_t i, float block_height) noexcept
    {
        block_heights_.Set(i, block_height);
    }

    // 全 N ノードの block_height を O(N) で一括再構築する。values.size() == size() 必須。
    void BuildBlockHeights(std::span<const float> values) noexcept
    {
        block_heights_.Build(values);
    }

    // ノード i のブロック上端 Y を Fenwick から O(log N) で取得する。
    // ここで「ブロック上端」とは spacing_above を含まない手前の位置で、
    // テキスト上端 (entry.text_top) は GetBlockTop(i) + spacing_above[i] と一致する。
    float GetBlockTop(size_t i, float margin_top) const noexcept
    {
        return margin_top + block_heights_.PrefixSum(i);
    }

    // ノード i の block_height (= spacing_above + height + spacing_below) を Fenwick から取得する。
    float GetBlockHeight(size_t i) const noexcept
    {
        return block_heights_.GetPoint(i);
    }

    // 文書 layout の総高さ (上下マージン + 全ノードの block_height 合計) を Fenwick から O(log N) で取得する。
    // = 2 * margin_top + sum(spacing_above[i] + height[i] + spacing_below[i]) for i in [0, N)
    // 用途: テーマ変更時の layout 健全性検査、effects_generation 更新の差分検知。
    // 注意: スクロール上限には ComputeTotalContentHeight (sb[last] を含まない) を使うこと。
    // 末尾の spacing_below は viewport 外の余白扱いのため、スクロール先には含めない。
    float GetTotalHeightFromFenwick(float margin_top) const noexcept
    {
        return margin_top * 2.0f + block_heights_.PrefixSum(block_heights_.size());
    }

private:
    // 列幅 / 行高さ / セルメトリクス系の派生キャッシュを既定値に戻す。
    // cell_inline_code_bgs は呼び出し側で必要に応じて別途クリアする
    // (DPI 変更パスは bg を作り直すまで維持する設計)。
    static void ResetTableLayoutGeometry(TableLayoutData& tl) noexcept;

    // table_layout / layout_dirty の扱いは呼び出し側ごとに異なるため触らない。
    static void ResetEntryTextLayout(NodeLayoutEntry& e) noexcept;

    static void EvictEntryLayout(NodeLayoutEntry& e) noexcept;

    // 1 行分の cell_layouts を Reset し、cell_heights / cell_applied_widths を再計測待ちに戻す。
    // 行/列の幾何 (row_cum_y, col_cum_x, row_heights, col_widths) は維持する。
    static void EvictTableRow(TableLayoutData& tl, size_t row_index) noexcept;

    std::pmr::vector<NodeLayoutEntry> entries_;
    std::pmr::vector<DiagramEntry> diagrams_;
    // 各ノードの block height (spacing_above + height + spacing_below) を保持する Fenwick tree。
    // RecomputeYPositions が更新し、GetBlockTop / GetTotalHeightFromFenwick で参照する。
    mendo::FloatFenwick block_heights_;
    uint32_t effects_generation_ = 0;
    size_t last_evict_fk_ = 0;
    size_t last_evict_lk_ = 0;
};

// 「コンテンツ末尾までの高さ」(末尾 node の text_top + height + 上端マージン)。
// = スクロール上限計算に使う高さ。末尾 node の spacing_below は含まない (= GetTotalHeightFromFenwick と sb[last] 分ずれる)。
// node_count > cache.size() の過渡状態 (doc 差し替え直後など) でも安全なよう effective にクランプする
// (FindFirstVisibleNodeIndex / EnsureScrollTarget と同じ防御)。effective が 0 なら 0 を返し underflow を回避。
constexpr float ComputeTotalContentHeight(const LayoutCache& cache, size_t node_count, float margin_top) noexcept
{
    const size_t effective = std::min(node_count, cache.size());
    if (effective == 0) {
        return 0.0f;
    }
    const size_t last = effective - 1;
    return cache[last].text_top + cache[last].height + margin_top;
}

// ノードの Y 範囲 [y, y+h] が [range_top, range_bottom] と重ならない場合 true を返す。
// 端が接している場合（y+h == range_top 等）は重なり扱い（false = 描画対象）。
// Why: ピクセル境界 (DIP→物理 px のスナップ後) で接する行は実際には 1 px 分視認可能で、
// `<` ではなく `<=` 不採用は浮動小数誤差で「微小に上回る」ケースを描画から落とすのを避けるため。
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
        return e.text_top + e.height <= viewport_top;
    });
    return static_cast<int>(it - first);
}

struct VisibleRange {
    size_t first;
    size_t last_plus_1;
};

// [first, last_plus_1) で range_top..range_bottom と重なる可能性があるノード範囲を返す。
// first: 下端が range_top 以上の最初のノード（FindFirstVisibleNodeIndex 相当）。
// last+1: 上端が range_bottom を超える最初のノード。
inline VisibleRange ComputeVisibleNodeRange(const LayoutCache& cache, size_t node_count,
                                            float range_top, float range_bottom) noexcept
{
    const size_t effective = std::min(node_count, cache.size());
    const size_t first = static_cast<size_t>(FindFirstVisibleNodeIndex(cache, effective, range_top));
    size_t last_plus_1 = first;
    for (size_t i = first; i < effective; ++i) {
        if (cache[i].text_top > range_bottom) {
            break;
        }
        last_plus_1 = i + 1;
    }
    return { first, last_plus_1 };
}

// (node, offset) → 絶対スクロール位置。
// 負の node や空キャッシュは 0 を返し、末尾を超える node は最後の要素へクランプする
constexpr float NodeOffsetToScrollY(const LayoutCache& cache, int node, float offset) noexcept
{
    if (cache.size() == 0 || node < 0) {
        return 0.0f;
    }
    const int clamped = std::min(node, static_cast<int>(cache.size()) - 1);
    return std::max(0.0f, cache[clamped].text_top + offset);
}
