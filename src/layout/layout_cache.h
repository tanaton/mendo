#pragma once
#include "document_types.h"
#include "png_bytes.h"
#include "ui_constants.h"
#include <cstdint>
#include <vector>
#include <memory>
#include <memory_resource>
#include <wrl/client.h>
#include <d2d1.h>
#include <dwrite.h>
#include <algorithm>
#include <ranges>
#include <span>
#include <unordered_map>
#include <utility>

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
    std::pmr::vector<uint8_t> row_links_applied; // 各行のリンク色適用済みフラグ (セル再生成で落ちる)
    std::pmr::vector<uint8_t> row_evicted;       // 行単位 evict でセル layout を捨てた行
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
    size_t evicted_row_count = 0;
    // セル layout が生存しうる行範囲 [live_row_begin, live_row_end)。差分 evict の走査範囲で、
    // evict 済み行を毎回なめ直さないためのもの。
    size_t live_row_begin = 0;
    size_t live_row_end = 0;

    constexpr bool HasEvictedRows() const noexcept
    {
        return evicted_row_count > 0;
    }
    // セルに適用した描画エフェクト (インラインコード背景・リンク色) の済み印を捨てる。
    void ClearEffects() noexcept
    {
        cell_inline_code_bgs.clear();
        row_bgs_computed.clear();
        row_links_applied.clear();
    }
    void ResetRowEviction(size_t row_count)
    {
        row_evicted.assign(row_count, 0);
        evicted_row_count = 0;
        live_row_begin = 0;
        live_row_end = row_count;
    }
    // セル単位の配列と row_evicted を row_count x col_count に揃えて確保し直す (セル layout は空)。
    // cell_layouts を最後に確保するので、途中で例外になっても「cell_layouts が非空なら
    // セル単位の配列と row_evicted のサイズが揃っている」ことは崩れない。
    void AllocateCells(size_t row_count, size_t new_col_count)
    {
        const size_t cell_count = row_count * new_col_count;
        cell_heights.assign(cell_count, 0.0f);
        cell_applied_widths.assign(cell_count, -1.0f);
        ResetRowEviction(row_count);
        col_count = new_col_count;
        cell_layouts.assign(cell_count, {});
    }
    // r が row_evicted の範囲内であることは呼び出し側が保証する。
    // row_links_applied はエフェクト未適用なら空なので範囲を確かめる。
    constexpr void MarkRowEvicted(size_t r) noexcept
    {
        if (!row_evicted[r]) {
            row_evicted[r] = 1;
            ++evicted_row_count;
        }
        if (r < row_links_applied.size()) {
            row_links_applied[r] = 0;
        }
    }
    constexpr void MarkRowRestored(size_t r) noexcept
    {
        if (row_evicted[r]) {
            row_evicted[r] = 0;
            --evicted_row_count;
        }
        if (live_row_begin >= live_row_end) {
            live_row_begin = r;
            live_row_end = r + 1;
        }
        else {
            live_row_begin = std::min(live_row_begin, r);
            live_row_end = std::max(live_row_end, r + 1);
        }
    }
    constexpr size_t CellIndex(size_t row, size_t col) const noexcept
    {
        return row * col_count + col;
    }

    IDWriteTextLayout* GetCellLayout(size_t row, size_t col) const noexcept
    {
        const size_t idx = CellIndex(row, col);
        return (idx < cell_layouts.size()) ? cell_layouts[idx].Get() : nullptr;
    }

    // [local_top, local_bottom] に重なる行帯 [r_begin, r_end) を二分探索で返す。
    // 座標はいずれもエントリ上端からのローカル系。row_cum_y が空なら [0, 0)。
    std::pair<size_t, size_t> VisibleRowRange(float local_top, float local_bottom) const noexcept;

    // row_cum_y が row_count 行分確定しているか (未計測なら false)。
    bool HasRowGeometry(size_t row_count) const noexcept
    {
        return row_cum_y.size() == row_count + 1;
    }

    // 列幅と col_cum_x が揃っているか。InvalidateGeometry の後は false。
    bool HasColGeometry() const noexcept
    {
        return !col_widths.empty() && col_cum_x.size() == col_widths.size() + 1;
    }

    // 描画に要る行・列の幾何がすべて揃っているか。FinalizeTableLayout で揃い、
    // 以後サイズは変わらないので、false になるのは InvalidateGeometry の後か計測が一度も完了していないときだけ。
    bool HasGeometry(size_t row_count, size_t expected_col_count) const noexcept
    {
        return expected_col_count > 0 && col_widths.size() == expected_col_count && HasColGeometry() &&
               row_heights.size() == row_count && HasRowGeometry(row_count);
    }

    // entry.height だけ推定値で伸ばしたときなど、列幅・行高さが entry と食い違う状態にする。
    // 次の計測で FinalizeTableLayout が作り直す。row_cum_y は行単位 evict の範囲計算に使うので残す。
    void InvalidateGeometry() noexcept
    {
        col_widths.clear();
        cached_table_width = 0.0f;
    }

    // 列 c の左端から次の列の左端までの幅 (列幅 + 左右パディング + 罫線)。col_cum_x の刻み幅。
    float ColumnStep(size_t c) const noexcept
    {
        return col_widths[c] + TABLE_CELL_PADDING * 2.0f + TABLE_BORDER_WIDTH;
    }

    // 行幾何が row_count と揃っていれば VisibleRowRange、未確定なら全行 [0, row_count)。
    std::pair<size_t, size_t> RowsInViewport(size_t row_count, float local_top, float local_bottom) const noexcept
    {
        if (!HasRowGeometry(row_count)) {
            return { 0, row_count };
        }
        return VisibleRowRange(local_top, local_bottom);
    }

    // local_y がヒットする行インデックスを返す。ヒットなしは -1。
    // 座標はエントリ上端からのローカル系。row_cum_y が空でもヒットなし扱い。
    int RowIndexAt(float local_y) const noexcept;
};

// セル枠の左端 (左罫線の右) と行上端 (上罫線の位置) から、セル本文の描画原点を求める。
// 描画 (GenTable) とヒットテストで同じ位置を使う。
constexpr D2D1_POINT_2F TableCellTextOrigin(float cell_left, float row_top) noexcept
{
    return { cell_left + TABLE_CELL_PADDING, row_top + TABLE_CELL_PADDING };
}

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

// unique_ptr の lazy 初期化ヘルパ。NodeLayoutEntry のキャッシュ群で共有する。
template <typename T>
constexpr T& EnsureUnique(std::unique_ptr<T>& p)
{
    if (!p) {
        p = std::make_unique<T>();
    }
    return *p;
}

} // namespace mendo::layout::detail

struct NodeLayoutEntry {
    // ノード Y 位置 (text_top) は LayoutCache::Top(i) に分離して持つ (SoA)。
    float height = 0.0f;
    // 直近 MeasureNode の幅と実測 height。同じ幅へ戻った際に EstimateNodeHeight の上書きを避ける。
    // 「未計測」のセンチネルは kUnmeasuredWidth (= 負値)。MeasureNode 入力 (node_width) は
    // 常に正なので負値は初期化/invalidate 経路でしか出現しない。per-node で密に並ぶ hot field の
    // ため optional<float> 化はサイズ膨張を招き避けている (is_measured() で判定統一)。
    static constexpr float kUnmeasuredWidth = -1.0f;
    float cached_width = kUnmeasuredWidth;
    float cached_height = 0.0f;
    constexpr bool is_measured() const noexcept
    {
        return cached_width > 0.0f;
    }
    bool layout_dirty = true;
    // 計測が例外で失敗した連続回数 (成功で 0 に戻る)。
    uint8_t measure_failures = 0;

    // 以降は計測・描画したノードでしか使わないので aux_ に置き、全ノード分は持たない。
    // 既定値を書く setter は aux_ を確保しない。
    constexpr IDWriteTextLayout* text_layout() const noexcept
    {
        return aux_ ? aux_->text_layout.Get() : nullptr;
    }
    void set_text_layout(Microsoft::WRL::ComPtr<IDWriteTextLayout> layout)
    {
        if (aux_ || layout) {
            ensure_aux().text_layout = std::move(layout);
        }
    }
    // 1 行目の高さ・自然幅・トークンは text_layout と一緒に作るので一緒に捨てる。
    void reset_text_layout() noexcept
    {
        if (aux_) {
            aux_->text_layout.Reset();
            aux_->first_line_height = 0.0f;
            aux_->natural_code_width = 0.0f;
            aux_->syntax_tokens = {};
        }
    }

    // CodeBlock の色付け用。計測ワーカーが text_layout と一緒に作り、ApplyNodeEffects が読む。
    std::span<const SyntaxToken> syntax_tokens() const noexcept
    {
        return aux_ ? std::span<const SyntaxToken>{ aux_->syntax_tokens } : std::span<const SyntaxToken>{};
    }
    void set_syntax_tokens(std::pmr::vector<SyntaxToken> tokens)
    {
        if (aux_ || !tokens.empty()) {
            ensure_aux().syntax_tokens = std::move(tokens);
        }
    }

    constexpr float first_line_height() const noexcept
    {
        return GetAux(&Aux::first_line_height);
    }
    constexpr void set_first_line_height(float h)
    {
        SetAux(&Aux::first_line_height, h);
    }
    constexpr float natural_code_width() const noexcept
    {
        return GetAux(&Aux::natural_code_width);
    }
    constexpr void set_natural_code_width(float w)
    {
        SetAux(&Aux::natural_code_width, w);
    }
    constexpr bool effects_applied() const noexcept
    {
        return GetAux(&Aux::effects_applied);
    }
    constexpr void set_effects_applied(bool applied)
    {
        SetAux(&Aux::effects_applied, applied);
    }
    constexpr bool inline_code_bgs_stale() const noexcept
    {
        return GetAux(&Aux::inline_code_bgs_stale);
    }
    constexpr void set_inline_code_bgs_stale(bool stale)
    {
        SetAux(&Aux::inline_code_bgs_stale, stale);
    }

    constexpr SearchHlCache& ensure_search_hl_cache() const
    {
        return mendo::layout::detail::EnsureUnique(ensure_aux().search_hl_cache);
    }

    constexpr void invalidate_search_hl_cache() const noexcept
    {
        if (aux_) {
            aux_->search_hl_cache.reset();
        }
    }

    constexpr SelectionHlCache& ensure_selection_hl_cache() const
    {
        return mendo::layout::detail::EnsureUnique(ensure_aux().selection_hl_cache);
    }

    constexpr void invalidate_selection_hl_cache() const noexcept
    {
        if (aux_) {
            aux_->selection_hl_cache.reset();
        }
    }

    // text_layout の wrap や format 属性が変わった際に、行折り返し由来のキャッシュ
    // (検索ハイライト矩形 / 選択ハイライト矩形) を一括で落とす。
    constexpr void invalidate_per_frame_hl_caches() const noexcept
    {
        invalidate_search_hl_cache();
        invalidate_selection_hl_cache();
    }

    // テーブルのみ確保。未確保なら nullptr。
    constexpr TableLayoutData* table_layout() const noexcept
    {
        return aux_ ? aux_->table_layout.get() : nullptr;
    }
    constexpr TableLayoutData& ensure_table_layout()
    {
        return mendo::layout::detail::EnsureUnique(ensure_aux().table_layout);
    }
    constexpr bool has_table_layout() const noexcept
    {
        return table_layout() != nullptr;
    }
    constexpr void reset_table_layout() noexcept
    {
        if (aux_) {
            aux_->table_layout.reset();
        }
    }

    constexpr std::pmr::vector<InlineCodeBg>& ensure_inline_code_bgs()
    {
        return ensure_aux().inline_code_bgs;
    }

    constexpr void clear_inline_code_bgs() noexcept
    {
        if (aux_) {
            aux_->inline_code_bgs.clear();
        }
    }

    constexpr std::span<const InlineCodeBg> view_inline_code_bgs() const noexcept
    {
        return aux_ ? std::span<const InlineCodeBg>{ aux_->inline_code_bgs } : std::span<const InlineCodeBg>{};
    }

    // テーブル幾何だけ残して、計測・描画で作った状態を捨てる。テーブル幾何も無ければ aux_ ごと解放する。
    void reset_layout_state() noexcept
    {
        if (!aux_) {
            return;
        }
        if (auto table = std::move(aux_->table_layout)) {
            *aux_ = Aux{};
            aux_->table_layout = std::move(table);
        }
        else {
            aux_.reset();
        }
    }
    // 計測・描画で作った状態をテーブル幾何も含めて捨てる。
    constexpr void release_layout_state() noexcept
    {
        aux_.reset();
    }

    // マッチの絶対 Y とその行の高さを返す。text_layout（テーブルはセル layout）が
    // あれば text_offset_w (UTF-16 code unit) に対応する行 Y を精密に計算し、無ければ
    // ブロック先頭/テーブル行先頭の座標にフォールバックする。
    // Why: 長い段落内の複数マッチで同じブロック先頭 Y に丸まると「次へ」でスクロールしない。
    // text_offset_w は IDWriteTextLayout::HitTestTextPosition の引数なので UTF-16 単位を渡すこと
    // (UTF-8 byte を渡すと非 ASCII を含む段落で行 Y がずれる)。
    std::pair<float, float> GetMatchYRange(int table_row, int table_col, uint32_t text_offset_w, float entry_text_top) const noexcept;

private:
    struct Aux {
        Microsoft::WRL::ComPtr<IDWriteTextLayout> text_layout;
        // GetLineMetrics(&lm, 1, &lc) の結果をキャッシュ。0 なら未確定 (フォールバック計算する)。
        // MeasureNode で text_layout 確定時に同時に求める。
        float first_line_height = 0.0f;
        // CodeBlock の最長行幅 (NO_WRAP で計測した自然幅)。横スクロールバーのスケール計算と
        // クランプに使う。CodeBlock 以外では 0 のまま。
        float natural_code_width = 0.0f;
        bool effects_applied = false;
        // SetMaxWidth だけの再計測は描画エフェクト/下線を保持するため、折り返しに依存する
        // インラインコード背景だけ作り直させる。effects_applied が true のときのみ意味を持つ。
        bool inline_code_bgs_stale = false;
        // 再計測のたびに作り直すので、確保し直さないよう Aux に直接持つ。
        std::pmr::vector<InlineCodeBg> inline_code_bgs;
        std::pmr::vector<SyntaxToken> syntax_tokens;
        // evict しても列幅・行高さを残すため、Aux を作り直しても引き継げるよう別に確保する。
        std::unique_ptr<TableLayoutData> table_layout;
        std::unique_ptr<SearchHlCache> search_hl_cache;
        std::unique_ptr<SelectionHlCache> selection_hl_cache;
    };

    constexpr Aux& ensure_aux() const
    {
        return mendo::layout::detail::EnsureUnique(aux_);
    }

    template <class T>
    constexpr T GetAux(T Aux::*field) const noexcept
    {
        return aux_ ? (*aux_).*field : T{};
    }
    template <class T>
    constexpr void SetAux(T Aux::*field, T value)
    {
        if (aux_ || value != T{}) {
            ensure_aux().*field = value;
        }
    }

    // 描画中に const 経由でハイライトキャッシュを確保するため mutable。
    mutable std::unique_ptr<Aux> aux_;
};

// 全ノード分並ぶので、計測・描画したノードでしか使わないフィールドは Aux に置いて増やさない。
static_assert(sizeof(NodeLayoutEntry) <= 24, "NodeLayoutEntry size regression: move per-materialized fields into Aux");

struct DiagramEntry {
    Microsoft::WRL::ComPtr<ID2D1Bitmap> bitmap;
    float width = 0.0f;
    float height = 0.0f;
    // クリップボードコピー用の元 PNG。bitmap と同時に設定/破棄されるため (EvictBitmap 含む)、
    // コピーボタンの表示条件 (bitmap 有無) とデータの有無が常に一致する。
    PngBytes png;
    // レンダリング失敗時のエラーメッセージ (表示用整形済み)。非空 = 失敗確定で、
    // プレースホルダにこの文言を表示し再リクエストを止める。リロード・幅変更・
    // テーマ変更でクリアされ再試行の契機になる。オフスクリーン evict では保持する。
    std::pmr::wstring error;

    // 未確定 (成功も失敗もしていない) でレンダ要求すべき状態か。
    bool NeedsRender() const noexcept
    {
        return !bitmap && error.empty();
    }

    // オフスクリーン evict 用。寸法とエラーはレイアウト維持/再試行抑止のため残す。
    void EvictBitmap() noexcept
    {
        bitmap.Reset();
        png.reset();
    }

    // レンダ結果 (成功/失敗とも) を破棄し再試行可能に戻す。幅変更・テーマ変更用。
    // オフスクリーン evict は bitmap のみ破棄する別意図なのでこれを使わない。
    void ResetForRetry() noexcept
    {
        bitmap.Reset();
        width = 0.0f;
        height = 0.0f;
        png.reset();
        error.clear();
    }
};

class LayoutCache {
public:
    void Resize(size_t node_count);

    void Reset(size_t node_count);

    constexpr size_t size() const noexcept
    {
        return entries_.size();
    }

    constexpr NodeLayoutEntry& operator[](size_t i) noexcept
    {
        return entries_[i];
    }
    constexpr const NodeLayoutEntry& operator[](size_t i) const noexcept
    {
        return entries_[i];
    }

    // ノード Y 位置の唯一の真実。WRITE 経路は RecomputeYPositions / ComputeLayout /
    // EstimateNodeHeights のみ。MeasureNode は触らない (= 中間状態は古い値のまま)。
    // 高さ変化時の後続ノード一括シフトが AoS のエントリを全件なめないよう、
    // 連続 float 配列として entries_ とは別に持つ。
    constexpr float Top(size_t i) const noexcept
    {
        return tops_[i];
    }
    constexpr void SetTop(size_t i, float y) noexcept
    {
        tops_[i] = y;
    }
    constexpr float Bottom(size_t i) const noexcept
    {
        return tops_[i] + entries_[i].height;
    }
    void ShiftTops(size_t from, float delta) noexcept
    {
        for (float& top : std::span(tops_).subspan(from)) {
            top += delta;
        }
    }

    // 画像/図のノードでしか使わないので全ノード分は持たない。確保は結果を書き込む EnsureDiagram だけが行い、
    // 読み取り・破棄は未確保 (nullptr) を返す FindDiagram を使う。
    // 要素は node-based のマップに置くので、Mermaid の処理待ちが持つポインタは他ノードの追加で動かない。
    DiagramEntry& EnsureDiagram(size_t i)
    {
        return diagrams_[i];
    }
    auto* FindDiagram(this auto& self, size_t i) noexcept
    {
        const auto it = self.diagrams_.find(i);
        return it != self.diagrams_.end() ? &it->second : nullptr;
    }

    // すべてのテキストレイアウトとエフェクトを無効化する（ズーム変更時）。
    // ダイアグラム/Mermaid キャッシュの処理は呼び出し側で別途行うこと。
    void InvalidateAllLayouts(const std::pmr::vector<Node>& nodes) noexcept;

    // 文字幾何 (IDWriteTextLayout / table cell layouts) は維持し、ApplyEffects を再走らせるため
    // effects_applied フラグとエフェクト由来のキャッシュだけ落とす。デバイスロスト時用。
    void InvalidateEffects() noexcept;

    // デバイスロスト時用。旧デバイス上のビットマップは新 RT で描画できず、
    // 残すと EndDraw が D2DERR_WRONG_RESOURCE_DOMAIN で失敗し続ける。
    // 画像の寸法は再計測が表示高さに使うので残す。
    void InvalidateAllDiagramBitmaps(const std::pmr::vector<Node>& nodes) noexcept;

    // ダイアグラム系ノードのビットマップを無効化する。
    // ダークモード切替時に text_layout とエフェクトを保持したまま図だけ再描画したい場合に使う。
    void InvalidateDiagramBitmaps(const std::pmr::vector<Node>& nodes) noexcept;

    // [first_keep_inclusive, last_keep_exclusive) の外にある画像/図のビットマップを捨てる。寸法とエラーは残す。
    void EvictDiagramBitmapsOutside(size_t first_keep_inclusive, size_t last_keep_exclusive) noexcept;

    // [0, first_keep_inclusive) と [last_keep_exclusive, size) のノードの text_layout / inline_code_bgs /
    // search_hl_cache を破棄し、layout_dirty=true で再生成させる。text_top と height は維持する。
    // 行追跡のあるテーブルは幾何を残してセルのレイアウトだけ捨て、dirty にはしない。レイアウトを持たないノードも dirty にはしない。
    // Why: IDWriteTextLayout は内部で glyph buffer / bidi 解析を保持し
    // 1K 文字あたり 2-5KB。可視外も保持し続けると巨大ドキュメントで数十 MB
    // のメモリを COM 側に常駐させてしまう。EVICT_BUFFER_SCREENS 分の安全マージン
    // を取った上で範囲外を解放する。呼び出し側（ResourceManager）は再描画時の
    // 再生成コストを踏まえてタイマー駆動で間引く。
    void EvictTextLayouts(size_t first_keep_inclusive, size_t last_keep_exclusive) noexcept;

    // 計測で layout を生成しうる index 範囲を記録する。EvictTextLayouts の差分走査は前回 keep 外を
    // 解放済みとみなすため、evict の合間に keep 外で計測した分 (スクロール先や全件ダーティ処理) をここで拾う。
    // UI スレッド専用。並列計測は呼び出し側が index 範囲をまとめて記録する。
    constexpr void NoteMaterialized(size_t first, size_t last_inclusive) noexcept
    {
        materialized_begin_ = std::min(materialized_begin_, first);
        materialized_end_ = std::max(materialized_end_, last_inclusive + 1);
    }
    constexpr void NoteMaterialized(size_t i) noexcept
    {
        NoteMaterialized(i, i);
    }

    // 計測が例外で失敗し続けるノードの試行回数の上限 (NodeLayoutEntry::measure_failures)。
    // 超えたら dirty を外して諦め、ダーティ処理タイマーが同じノードを毎 tick 計測し続けるのを止める。
    static constexpr uint8_t kMaxMeasureAttempts = 3;

    // 全 text_layout を破棄する操作の後に呼ぶ。差分エビクトが「破棄済み」と誤認して
    // 再生成済みの画面外レイアウトを取り逃がさないよう、追跡境界を初期化する。
    void ResetEvictionTracking() noexcept;

    // 可視範囲をまたぐテーブルの可視外行で cell_layouts を Reset する。
    // 再表示時は LayoutEngine::EnsureVisibleLayout の行復元で CreateTextLayout を再発行する。
    // viewport は文書グローバル座標で渡す (entry ローカル座標ではない)。
    // table_indices は Document::GetTableNodeIndices() のうち keep 範囲と交差するもの。全エントリ走査だと 100MB 級
    // 文書でスクロール休止のたびに ~100万エントリを読むため、index リストで絞る。
    void EvictInvisibleTableRows(std::span<const size_t> table_indices, float viewport_top, float viewport_bottom, float buffer_screens_height) noexcept;

    // DPI 変更時の最小リセット。IDWriteTextLayout は DIP 単位なので不変、ピクセルに依存するハイライト矩形だけ捨てる。
    void NotifyDpiChanged() noexcept;

private:
    std::pmr::vector<NodeLayoutEntry> entries_;
    std::pmr::vector<float> tops_;
    std::pmr::unordered_map<size_t, DiagramEntry> diagrams_;
    size_t last_evict_fk_ = 0;
    size_t last_evict_lk_ = 0;
    // 前回 evict 以降に計測した index の範囲 [begin, end)。空は begin >= end。
    size_t materialized_begin_ = SIZE_MAX;
    size_t materialized_end_ = 0;
};

// 「コンテンツ末尾までの高さ」(末尾 node の text_top + height + 下端マージン)。
// = スクロール上限計算に使う高さ。末尾 node の spacing_below は含まない。空なら 0。
constexpr float ComputeTotalContentHeight(const LayoutCache& cache, float margin_bottom) noexcept
{
    if (cache.size() == 0) {
        return 0.0f;
    }
    return cache.Bottom(cache.size() - 1) + margin_bottom;
}

// ノードの Y 範囲 [y, y+h] が [range_top, range_bottom] と重ならない場合 true を返す。
// 端が接している場合（y+h == range_top 等）は重なり扱い（false = 描画対象）。
// Why: ピクセル境界 (DIP→物理 px のスナップ後) で接する行は実際には 1 px 分視認可能なため。
constexpr bool IsOffscreen(float y, float h, float range_top, float range_bottom) noexcept
{
    return y + h < range_top || y > range_bottom;
}

// 下端が viewport_top 以上の最初のノードを二分探索で見つける。該当なしの場合は cache.size() を返す。
constexpr int FindFirstVisibleNodeIndex(const LayoutCache& cache, float viewport_top) noexcept
{
    const auto indices = std::views::iota(size_t{ 0 }, cache.size());
    const auto it = std::ranges::partition_point(indices, [&cache, viewport_top](size_t i) noexcept {
        return cache.Bottom(i) <= viewport_top;
    });
    return static_cast<int>(it == indices.end() ? cache.size() : *it);
}

struct VisibleRange {
    size_t first;
    size_t last_plus_1;
};

// [first, last_plus_1) で range_top..range_bottom と重なる可能性があるノード範囲を返す。
// first: 下端が range_top 以上の最初のノード（FindFirstVisibleNodeIndex 相当）。
// last+1: 上端が range_bottom を超える最初のノード。
inline VisibleRange ComputeVisibleNodeRange(const LayoutCache& cache, float range_top, float range_bottom) noexcept
{
    const size_t first = static_cast<size_t>(FindFirstVisibleNodeIndex(cache, range_top));
    size_t last_plus_1 = first;
    for (size_t i = first; i < cache.size(); ++i) {
        if (cache.Top(i) > range_bottom) {
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
    return std::max(0.0f, cache.Top(static_cast<size_t>(clamped)) + offset);
}
