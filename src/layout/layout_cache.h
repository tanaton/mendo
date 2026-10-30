#pragma once
#include "document_types.h"
#include "ui_constants.h"
#include "utility.h"
#include <cassert>
#include <cstdint>
#include <limits>
#include <vector>
#include <memory>
#include <wrl/client.h>
#include <d2d1.h>
#include <dwrite.h>
#include <algorithm>
#include <ranges>
#include <span>
#include <unordered_map>
#include <utility>

// インラインコードの背景矩形（字間のパディング込み、レイアウト原点からの相対座標）
using InlineCodeBg = D2D1_RECT_F;

// テーブル内インラインコード背景の 1 エントリ。行ごとに列の昇順で並ぶ。
struct CellInlineCodeBg {
    uint32_t col;
    InlineCodeBg rect; // セルのテキスト原点基準
};

// 表の幅の差がこれ未満なら、前回の計測 (セル高さ・列幅) を再利用する。
inline constexpr float CELL_WIDTH_EPSILON = 0.5f;

// 捨てたテキストレイアウト。1 個の解放に数 µs かかり、ズームや巨大な表の evict では数千〜数十万個になるので、
// UI スレッドで解放せず、呼び出し側が受け取ってワーカーで解放する。
using ReleasedLayouts = std::vector<Microsoft::WRL::ComPtr<IDWriteTextLayout>>;

// テーブル専用のレイアウトデータ（テーブルノードのみ確保してメモリを節約）。
// エントリは中身の同じノードの間でしか移らない (TakeEntries) ので、行数・列数は作ったときに決まり変わらない。
struct TableLayoutData {
    const size_t row_count = 0;
    const size_t col_count = 0;                                               // cell_layouts のストライド
    std::vector<Microsoft::WRL::ComPtr<IDWriteTextLayout>> cell_layouts; // フラット配列 [行 * col_count + 列]
    // 行ごとのインラインコード背景。インラインコードを含むテーブルだけ row_count 分確保し、行の計測で作り直す。
    std::vector<std::vector<CellInlineCodeBg>> row_code_bgs;
    std::vector<float> col_widths;
    std::vector<float> row_heights;
    std::vector<float> natural_col_widths;  // リサイズ高速パス用キャッシュ
    std::vector<float> cell_natural_widths; // 各セルの折り返さない幅。テキストは変わらないので evict しても残す
    std::vector<float> cell_nowrap_heights; // 各セルの折り返さないときの高さ。evict 中の行の高さを出すのに使う
    // ヒットテスト高速化用の累積オフセット。最初の FinalizeTableLayout までは空。
    // row_cum_y[r] = エントリ上端からの行 r の上端までの累積高さ。サイズは row_count+1。
    // col_cum_x[c] = base_x からの列 c の左端までの累積幅。サイズは col_count+1。
    std::vector<float> row_cum_y;
    std::vector<float> col_cum_x;
    // 幾何を組んだ max_width (CommitGeometry)。GeometryValidAt で比べる。
    float last_applied_max_width = 0.0f;
    // NodeLayoutEntry::pending_math のセル版。セルは並列に作るので std::atomic_ref で書く。
    bool pending_math = false;

    TableLayoutData() = default;
    // セル単位・行単位の配列を表の大きさで確保する (セル layout は空)。
    TableLayoutData(size_t rows, size_t cols, bool has_inline_code)
        : row_count(rows), col_count(cols), cell_layouts(rows * cols), row_code_bgs(has_inline_code ? rows : 0),
          row_heights(rows), natural_col_widths(cols), cell_natural_widths(rows * cols), cell_nowrap_heights(rows * cols),
          row_evicted_(rows), live_row_end_(rows)
    {
    }

    constexpr bool HasEvictedRows() const noexcept
    {
        return evicted_row_count_ > 0;
    }
    constexpr size_t EvictedRowCount() const noexcept
    {
        return evicted_row_count_;
    }
    constexpr bool IsRowEvicted(size_t r) const noexcept
    {
        return row_evicted_[r] != 0;
    }
    // セル layout を持ちうる行があるか。
    // セル layout が生存しうる行範囲 [begin, end)。外の行は layout を持たない。
    constexpr std::pair<size_t, size_t> LiveRowRange() const noexcept
    {
        return { live_row_begin_, live_row_end_ };
    }
    constexpr bool HasLiveRows() const noexcept
    {
        return live_row_begin_ < live_row_end_;
    }
    // 全行がセル layout を持たない状態にする。行は可視になったときに LayoutEngine::MeasurePass の行復元が作る。
    void MarkAllRowsEvicted() noexcept
    {
        std::ranges::fill(row_evicted_, uint8_t{ 1 });
        evicted_row_count_ = row_count;
        live_row_begin_ = 0;
        live_row_end_ = 0;
    }
    constexpr void MarkRowRestored(size_t r) noexcept
    {
        if (row_evicted_[r]) {
            row_evicted_[r] = 0;
            --evicted_row_count_;
        }
        if (!HasLiveRows()) {
            live_row_begin_ = r;
            live_row_end_ = r + 1;
        }
        else {
            live_row_begin_ = std::min(live_row_begin_, r);
            live_row_end_ = std::max(live_row_end_, r + 1);
        }
    }
    // 生存範囲のうち [keep_begin, keep_end) の外の行のセル layout を released へ移し、生存範囲を keep との重なりに縮める。
    // evict 済みの行はなめ直さない。行・列の幾何は残す。HasGeometry() のときだけ呼ぶ。
    void EvictRowsOutside(size_t keep_begin, size_t keep_end, ReleasedLayouts& released) noexcept;
    constexpr size_t CellIndex(size_t row, size_t col) const noexcept
    {
        return row * col_count + col;
    }

    IDWriteTextLayout* GetCellLayout(size_t row, size_t col) const noexcept
    {
        return cell_layouts[CellIndex(row, col)].Get();
    }

    // [local_top, local_bottom] に重なる行帯 [r_begin, r_end) を二分探索で返す。
    // 座標はいずれもエントリ上端からのローカル系。row_cum_y が空なら [0, 0)。
    std::pair<size_t, size_t> VisibleRowRange(float local_top, float local_bottom) const noexcept;
    // x は列順に単調なので、[local_left, local_right] に重なる列も連続した区間 [c_begin, c_end) になる。
    // 座標は col_cum_x と同じ base_x からのローカル系。
    std::pair<size_t, size_t> VisibleColRange(float local_left, float local_right) const noexcept;

    // 行・列の幾何 (row_cum_y・col_cum_x) が積まれているか (未計測なら false)。積むのは全セルを測り終えた後
    // (MeasureTableCells → CommitGeometry) だけなので、全セルの自然幅と折り返さない高さが揃っていることも表す。
    // 幅が変わっても次に組み直すまでは前の幅の幾何のまま残る (見えている表は描く前に組み直す)。
    bool HasGeometry() const noexcept
    {
        return !row_cum_y.empty();
    }
    // 幾何が揃っていて、max_width との差が CELL_WIDTH_EPSILON 未満 (組み直さなくてよい)。
    bool GeometryValidAt(float max_width) const noexcept
    {
        return HasGeometry() && std::abs(last_applied_max_width - max_width) < CELL_WIDTH_EPSILON;
    }

    // row_heights / col_widths から累積位置を積み、max_width で組んだ幾何として確定させる。
    void CommitGeometry(float max_width)
    {
        RebuildRowCumY(0);
        RebuildColCumX();
        last_applied_max_width = max_width;
    }

    // 列幅 + パディング + 罫線の合計。HasGeometry() が前提。
    float TableWidth() const noexcept
    {
        return col_cum_x.back();
    }

    // 列 c の左端から次の列の左端までの幅 (列幅 + 左右パディング + 罫線)。col_cum_x の刻み幅。
    float ColumnStep(size_t c) const noexcept
    {
        return col_widths[c] + TABLE_CELL_PADDING * 2.0f + TABLE_BORDER_WIDTH;
    }

    // row_heights から罫線込みの累積位置を積み直す。
    // first_row より前の row_cum_y は有効なものとして使い回す (まだ積んでいなければ全行)。
    void RebuildRowCumY(size_t first_row);

    // 最下行の下罫線まで含めた表の高さ (計測で entry.height に確定する値)。HasGeometry() が前提。
    float TotalHeight() const noexcept
    {
        return row_cum_y[row_count] + TABLE_BORDER_WIDTH;
    }

    // local_y がヒットする行インデックスを返す。ヒットなしは -1。
    // 座標はエントリ上端からのローカル系。row_cum_y が空でもヒットなし扱い。
    int RowIndexAt(float local_y) const noexcept;

private:
    void RebuildColCumX();
    // 背景は幅が変わらない限り有効なので残す (復元時の行の計測で作り直す)。
    void EvictRow(size_t r, ReleasedLayouts& released) noexcept;

    std::vector<uint8_t> row_evicted_; // 行単位 evict でセル layout を捨てた行
    size_t evicted_row_count_ = 0;
    // セル layout が生存しうる行範囲 [live_row_begin_, live_row_end_)。差分 evict の走査範囲で、
    // evict 済み行を毎回なめ直さないためのもの。
    size_t live_row_begin_ = 0;
    size_t live_row_end_ = 0;
};

// セル枠の左端 (左罫線の右) と行上端 (上罫線の位置) から、セル本文の描画原点を求める。
// 描画 (GenTable) とヒットテストで同じ位置を使う。
constexpr D2D1_POINT_2F TableCellTextOrigin(float cell_left, float row_top) noexcept
{
    return { cell_left + TABLE_CELL_PADDING, row_top + TABLE_CELL_PADDING };
}

// Y の閉区間 [top, bottom]。文書座標でも、エントリ上端からのローカル座標でも使う。
// 「全部」と「空」のどちらにもなりうるので既定値は持たせず、All() / None() で明示する。
struct YRange {
    static constexpr float kInf = std::numeric_limits<float>::infinity();
    float top;
    float bottom;

    constexpr YRange(float t, float b) noexcept : top(t), bottom(b)
    {
    }
    // どの Y も含む。帯の二分探索・下端 break がすべて素通りになる。
    static constexpr YRange All() noexcept
    {
        return { -kInf, kInf };
    }
    // どの範囲も含まない。
    static constexpr YRange None() noexcept
    {
        return { kInf, -kInf };
    }

    constexpr bool Contains(const YRange& r) const noexcept
    {
        return top <= r.top && r.bottom <= bottom;
    }
    constexpr YRange ToLocal(float entry_top) const noexcept
    {
        return { top - entry_top, bottom - entry_top };
    }
};

namespace layout {

// scroll_y から height の可視範囲に、上下 height * screens ずつ足した範囲。
constexpr YRange BufferedRange(float scroll_y, float height, float screens) noexcept
{
    const float buffer = height * screens;
    return { scroll_y - buffer, scroll_y + height + buffer };
}

} // namespace layout

// 本文 text_layout のうち、ローカル Y 範囲 y にかかる行のテキスト範囲 [text_begin, text_end) (UTF-16)。
// 検索・選択ハイライトが共有し、可視範囲が y に収まる間は引き直さない。
struct LineWindow {
    YRange y = YRange::None();
    uint32_t text_begin = 0;
    uint32_t text_end = 0;
};

// 検索ハイライト矩形のフレーム間キャッシュ。マッチは可視範囲に入ったものだけ計算する。
// ノード内マッチ i の矩形は rects[spans[i].begin, spans[i].begin + spans[i].count)。
// gen が SearchState の generation と一致する間は計算済みの矩形を使い回す。
struct SearchHlCache {
    struct Span {
        static constexpr uint32_t NOT_COMPUTED = UINT32_MAX;
        uint32_t begin = 0;
        uint32_t count = NOT_COMPUTED;
    };
    std::vector<D2D1_RECT_F> rects;
    std::vector<Span> spans;
    uint32_t gen = 0;
};

// 選択ハイライト矩形のフレーム間キャッシュ。本文ノード（テーブル外）でのみ使用。
// rects は選択範囲を LineWindow で切り詰めた [rects_begin_w, rects_end_w) の分だけを持ち、
// 切り詰めた範囲が変わらない間は HitTestTextRange 再計算をスキップする。
// (start, length) は UTF-8 → UTF-16 変換の結果 [start_w, end_w) を使い回すためのキー。
// text_layout 失効時は invalidate_selection_hl_cache() で unique_ptr ごと破棄する契約なので、layout はキーに含めない。
struct SelectionHlCache {
    std::vector<D2D1_RECT_F> rects;
    uint32_t start = 0;
    uint32_t length = 0;
    uint32_t start_w = 0;
    uint32_t end_w = 0;
    uint32_t rects_begin_w = 0;
    uint32_t rects_end_w = 0;
};

namespace layout::detail {

// unique_ptr の lazy 初期化ヘルパ。NodeLayoutEntry のキャッシュ群で共有する。
template <typename T>
constexpr T& EnsureUnique(std::unique_ptr<T>& p)
{
    if (!p) {
        p = std::make_unique<T>();
    }
    return *p;
}

} // namespace layout::detail

struct NodeLayoutEntry {
    // ノード Y 位置 (text_top) は LayoutCache::Top(i) に分離して持つ (SoA)。
    float height = 0.0f;
    // 直近 MeasureNode の幅と実測 height。同じ幅へ戻った際に EstimateInvisibleNodeHeight の上書きを避ける。
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
    // 計測に失敗した (例外か、測れなかった) 連続回数 (成功で 0 に戻る)。
    uint8_t measure_failures = 0;
    // 寸法がまだ無いインライン数式を原文のまま組んだ。描画の結果が出たら組み直す (LayoutCache::InvalidatePendingMathLayouts)。
    bool pending_math = false;

    // 計測した高さを確定する。height・layout_dirty・cached_*・measure_failures を計測の結果として書くのは
    // ここと RecordMeasureFailure だけ。layout_width は組んだ幅 (NodeLayoutWidth)。
    constexpr void CommitMeasured(float layout_width, float measured_height) noexcept
    {
        height = measured_height;
        layout_dirty = false;
        cached_width = layout_width;
        cached_height = measured_height;
        measure_failures = 0;
    }
    // 測れなかったことを記録し、まだ再試行するなら true。上限に達したら dirty を外して諦め、
    // 描画のたびに同じノードを測り直して失敗し続けない。諦めた後に再び dirty になって失敗しても一周しないよう頭打ちにする。
    constexpr bool RecordMeasureFailure(uint8_t max_attempts) noexcept
    {
        if (measure_failures < max_attempts) {
            ++measure_failures;
        }
        if (measure_failures >= max_attempts) {
            layout_dirty = false;
            return false;
        }
        return true;
    }

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

    // CodeBlock の色付け用。計測ワーカーが text_layout と一緒に作り、描画時にグリフランの色分けに使う。
    // 位置は UTF-16 単位。トークン間の空白だけの隙間は直前のトークンに含め、同種のトークンはまとめてある。
    std::span<const SyntaxToken> syntax_tokens() const noexcept
    {
        return aux_ ? std::span<const SyntaxToken>{ aux_->syntax_tokens } : std::span<const SyntaxToken>{};
    }
    void set_syntax_tokens(std::vector<SyntaxToken> tokens)
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

    constexpr SearchHlCache& ensure_search_hl_cache() const
    {
        return layout::detail::EnsureUnique(ensure_aux().search_hl_cache);
    }

    constexpr void invalidate_search_hl_cache() const noexcept
    {
        if (aux_) {
            aux_->search_hl_cache.reset();
        }
    }

    constexpr SelectionHlCache& ensure_selection_hl_cache() const
    {
        return layout::detail::EnsureUnique(ensure_aux().selection_hl_cache);
    }

    constexpr void invalidate_selection_hl_cache() const noexcept
    {
        if (aux_) {
            aux_->selection_hl_cache.reset();
        }
    }

    constexpr LineWindow& ensure_line_window() const
    {
        return layout::detail::EnsureUnique(ensure_aux().line_window);
    }

    // text_layout の wrap や format 属性が変わった際に、行折り返し由来のキャッシュ
    // (検索ハイライト矩形 / 選択ハイライト矩形 / 可視行のテキスト範囲) を一括で落とす。
    constexpr void invalidate_per_frame_hl_caches() const noexcept
    {
        invalidate_search_hl_cache();
        invalidate_selection_hl_cache();
        if (aux_) {
            aux_->line_window.reset();
        }
    }

    // テーブルのみ確保。未確保なら nullptr。
    constexpr TableLayoutData* table_layout() const noexcept
    {
        return aux_ ? aux_->table_layout.get() : nullptr;
    }
    // 表の大きさは最初に確保したときに決まる。has_inline_code は確保するときだけ見る。
    TableLayoutData& ensure_table_layout(size_t row_count, size_t col_count, bool has_inline_code = false)
    {
        auto& table = ensure_aux().table_layout;
        if (!table) {
            table = std::make_unique<TableLayoutData>(row_count, col_count, has_inline_code);
        }
        assert(table->row_count == row_count && table->col_count == col_count);
        return *table;
    }

    constexpr std::vector<InlineCodeBg>& ensure_inline_code_bgs()
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

    // 計測・描画で作った状態をテーブル幾何も含めて捨てる。
    constexpr void release_layout_state() noexcept
    {
        aux_.reset();
    }
    Microsoft::WRL::ComPtr<IDWriteTextLayout> take_text_layout() noexcept
    {
        return aux_ ? std::move(aux_->text_layout) : nullptr;
    }

    // マッチの絶対 Y とその行の高さを返す。text_layout（テーブルはセル layout）が
    // あれば text_offset_w (UTF-16 code unit) に対応する行 Y を精密に計算し、無ければ
    // ブロック先頭/テーブル行先頭の座標にフォールバックする。
    // Why: 長い段落内の複数マッチで同じブロック先頭 Y に丸まると「次へ」でスクロールしない。
    // text_offset_w は IDWriteTextLayout のヒットテストに渡すので UTF-16 単位を渡すこと
    // (UTF-8 byte を渡すと非 ASCII を含む段落で行 Y がずれる)。
    std::pair<float, float> GetMatchYRange(int table_row, int table_col, uint32_t text_offset_w, float entry_text_top) const noexcept;

private:
    struct Aux {
        Microsoft::WRL::ComPtr<IDWriteTextLayout> text_layout;
        // GetLineMetrics(&lm, 1, &lc) の結果をキャッシュ。0 なら未確定 (フォールバック計算する)。
        // MeasureNode で text_layout 確定時に同時に求める。
        float first_line_height = 0.0f;
        // CodeBlock の自然幅 (NO_WRAP で計測した最長行 + 左右の余白)。横スクロールバーのスケール計算と
        // クランプに使う。余白込みにしないと末尾までスクロールしても最長行の右端が clip に隠れる。
        // CodeBlock 以外では 0 のまま。
        float natural_code_width = 0.0f;
        // 計測のたびに作り直すので、確保し直さないよう Aux に直接持つ。
        std::vector<InlineCodeBg> inline_code_bgs;
        std::vector<SyntaxToken> syntax_tokens;
        // テーブルノードでしか使わないので別に確保する。
        std::unique_ptr<TableLayoutData> table_layout;
        // ハイライト系の 3 つは unique_ptr で持つ。Aux に直に持つと、リロードで全エントリを移す TakeEntries が
        // どのエントリも Aux を持たない文書でも遅くなった (Reload/CarryOverUnchanged 700→1100µs)。
        std::unique_ptr<SearchHlCache> search_hl_cache;
        std::unique_ptr<SelectionHlCache> selection_hl_cache;
        std::unique_ptr<LineWindow> line_window;
    };

    constexpr Aux& ensure_aux() const
    {
        return layout::detail::EnsureUnique(aux_);
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

// 本文 text_layout の、view (layout ローカル Y) の上下に 1 画面ずつ足した行のテキスト範囲。
// 同じフレームの検索・選択ハイライトが共有し、view が窓に収まる間は引き直さない。
const LineWindow& EnsureLineWindow(const NodeLayoutEntry& entry, YRange view);

// 図を描けなかった理由。言語で変わる文言は描くときに引き、言語を切り替えても前の言語の文言を残さない。
struct DiagramError {
    enum class Kind : uint8_t {
        None,
        // message を出す。空なら汎用の文言を出す。
        Render,
        TooLarge,
    };
    Kind kind = Kind::None;
    // Mermaid が返した文言 (表示用に整形済み)。Mermaid の文言は訳さないのでそのまま持つ。
    std::wstring message;

    bool empty() const noexcept
    {
        return kind == Kind::None;
    }
    void clear() noexcept
    {
        kind = Kind::None;
        message.clear();
    }
};

struct DiagramEntry {
    Microsoft::WRL::ComPtr<ID2D1Bitmap> bitmap;
    float width = 0.0f;
    float height = 0.0f;
    // クリップボードコピー・保存用の元 PNG。bitmap と同時に設定/破棄する (EvictBitmap 含む)。
    // コードを書き換えた図の仮表示では別の図の絵なので捨て、描き直すまでボタンを出さない。
    PngBytes png;
    // レンダリング失敗の理由。非空 = 失敗確定で、
    // プレースホルダにその文言を表示し再リクエストを止める。リロード・幅変更・
    // テーマ変更・デバイスロストでクリアされ再試行の契機になる。オフスクリーン evict では保持する。
    DiagramError error;
    // 図のコード部分のハッシュ (mermaid_util::DiagramCodeHash)。0 は未計算。ノードのテキストだけで決まるので、
    // 結果を捨てても (幅・テーマの変更を含む) 残し、結果を待つ図を要求し直すたびにテキストを読み直さない。
    uint64_t code_hash = 0;
    // bitmap・寸法・png は前のコード・幅・テーマで描いたもの。描き直しの間にプレースホルダへ戻って
    // 高さが 2 回変わらないよう、新しい結果が届くまで仮に表示する。
    bool stale = false;

    // 未確定 (成功も失敗もしていない) か、仮表示中でレンダ要求すべき状態か。
    bool NeedsRender() const noexcept
    {
        return (!bitmap || stale) && error.empty();
    }

    // オフスクリーン evict 用。寸法とエラーはレイアウト維持/再試行抑止のため残す。
    void EvictBitmap() noexcept
    {
        bitmap.Reset();
        png.reset();
        stale = false;
    }

    // デバイスロスト用。旧デバイスのビットマップと、旧デバイスで確定したかもしれないエラーを捨てる。
    // 寸法はレイアウト維持のため残す。
    void EvictForRetry() noexcept
    {
        EvictBitmap();
        error.clear();
    }

    // レンダ結果 (成功/失敗とも) を破棄し再試行可能に戻す。
    // オフスクリーン evict は bitmap のみ破棄する別意図なのでこれを使わない。
    void ResetForRetry() noexcept
    {
        bitmap.Reset();
        width = 0.0f;
        height = 0.0f;
        png.reset();
        error.clear();
        stale = false;
    }

    // 幅・テーマ・コードの変更用。描いた結果があれば仮表示として残して描き直させ、無ければ ResetForRetry と同じ。
    void MarkStale() noexcept
    {
        if (!bitmap) {
            ResetForRetry();
            return;
        }
        stale = true;
        error.clear();
    }
};

class LayoutCache {
public:
    LayoutCache() = default;
    // node_count 個の未計測 (dirty・位置 0) のエントリを持つ。文書を表示に載せる経路は MakeEstimatedLayoutCache で作る。
    explicit LayoutCache(size_t node_count)
        : entries_(node_count), tops_(node_count), top_block_offsets_((node_count >> kTopBlockShift) + 1, 0.0f)
    {
    }

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

    // ノード Y 位置の唯一の真実。WRITE 経路は layout_computer の RecomputeYPositions / EstimateNodeHeights /
    // ReestimateNodeHeights のみ。MeasureNode は触らない (= 中間状態は古い値のまま)。
    // 高さ変化時の後続ノード一括シフトが AoS のエントリを全件なめないよう、
    // 連続 float 配列として entries_ とは別に持つ。
    constexpr float Top(size_t i) const noexcept
    {
        return tops_[i] + top_block_offsets_[i >> kTopBlockShift];
    }
    constexpr void SetTop(size_t i, float y) noexcept
    {
        tops_[i] = y - top_block_offsets_[i >> kTopBlockShift];
    }
    constexpr float Bottom(size_t i) const noexcept
    {
        return Top(i) + entries_[i].height;
    }
    // from 以降の Top を delta ずらす。後続のブロックはオフセットだけ書き換える。
    void ShiftTops(size_t from, float delta) noexcept
    {
        if (from >= tops_.size()) {
            return;
        }
        const size_t block = from >> kTopBlockShift;
        const size_t block_end = std::min((block + 1) << kTopBlockShift, tops_.size());
        for (float& top : std::span(tops_).subspan(from, block_end - from)) {
            top += delta;
        }
        for (float& offset : std::span(top_block_offsets_).subspan(block + 1)) {
            offset += delta;
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

    // すべてのテキストレイアウトを無効化し、全ノードを dirty にして次の Reestimate に推定し直させる（ズーム変更時）。
    // ダイアグラム/Mermaid キャッシュの処理は呼び出し側で別途行うこと。
    void InvalidateAllLayouts(const std::vector<Node>& nodes) noexcept;

    // インライン数式の寸法を待って原文のまま組んだノードを dirty に戻し、組み直させる。1 つでもあれば true。
    // tables が false なら表は待たせたままにする (表は全セルを組み直すので重い)。
    bool InvalidatePendingMathLayouts(bool tables) noexcept;

    // デバイスロスト時用。旧デバイス上のビットマップは新 RT で描画できず、
    // 残すと EndDraw が D2DERR_WRONG_RESOURCE_DOMAIN で失敗し続ける。
    // 寸法は再計測が表示高さに使うので残し、旧デバイスで確定したエラーは再試行させる。
    void InvalidateAllDiagramBitmaps() noexcept;

    // 図のビットマップを再描画待ちに戻す。ダークモード切替時に text_layout を
    // 保持したまま図だけ再描画したい場合に使う。
    void InvalidateDiagramBitmaps(std::span<const size_t> diagram_indices) noexcept;

    // [first_keep_inclusive, last_keep_exclusive) の外にある画像/図のビットマップを捨てる。寸法とエラーは残す。
    void EvictDiagramBitmapsOutside(size_t first_keep_inclusive, size_t last_keep_exclusive) noexcept;

    // [0, first_keep_inclusive) と [last_keep_exclusive, size) のノードの text_layout / inline_code_bgs /
    // search_hl_cache を破棄し、layout_dirty=true で再生成させる。text_top と height は維持する。
    // 行追跡のあるテーブルは幾何を残してセルのレイアウトだけ捨て、dirty にはしない。レイアウトを持たないノードも dirty にはしない。
    // Why: IDWriteTextLayout は内部で glyph buffer / bidi 解析を保持し
    // 1K 文字あたり 2-5KB。可視外も保持し続けると巨大ドキュメントで数十 MB
    // のメモリを COM 側に常駐させてしまう。LAYOUT_EVICT_BUFFER_SCREENS 分の安全マージン
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

    // 計測に失敗し続けるノードの試行回数の上限 (NodeLayoutEntry::measure_failures)。
    // 超えたら dirty を外して諦め、遅延レイアウトが同じノードを毎回計測し続けるのを止める。
    static constexpr uint8_t kMaxMeasureAttempts = 3;

    // keep (文書座標) の外にあるテーブル行の cell_layouts を Reset する。
    // 再表示時は LayoutEngine::MeasurePass の行復元で CreateTextLayout を再発行する。
    // table_indices は Document::GetTableNodeIndices() のうち keep 範囲と交差するもの。全エントリ走査だと 100MB 級
    // 文書でスクロール休止のたびに ~100万エントリを読むため、index リストで絞る。
    void EvictInvisibleTableRows(std::span<const size_t> table_indices, YRange keep) noexcept;

    // src のノード [from, from + count) の計測結果 (高さ・テキストレイアウト・図) を、中身が同じノード
    // [to, to + count) へ移す。ハイライト系のキャッシュは選択や検索の位置に依るので捨てる。Y 位置は呼び出し側が組み直す。
    void TakeEntries(LayoutCache& src, size_t from, size_t to, size_t count);
    // src のノード from の図を、コードが書き換わったノード to へ仮表示として移す。高さも引き継いで表示を跳ねさせない。
    // 描いた結果が無ければ何もせず false を返す。
    bool TakeDiagramAsStale(LayoutCache& src, size_t from, size_t to);

    // 遅延レイアウトで計測する dirty が残っているかもしれない。LayoutEngine が計測のたびに決め直す。
    // キャッシュと一緒に動くので、文書と一緒に差し替えても (推定直後は true) 遅延レイアウトが拾う。
    // 画面外の evict で dirty に戻したノードは含めない (スクロールで見えたときに計測する)。
    constexpr bool HasDirtyNodes() const noexcept
    {
        return has_dirty_nodes_;
    }
    constexpr void SetHasDirtyNodes(bool v) noexcept
    {
        has_dirty_nodes_ = v;
    }
    bool HasReleasedLayouts() const noexcept
    {
        return !released_layouts_.empty();
    }
    ReleasedLayouts TakeReleasedLayouts() noexcept
    {
        return std::exchange(released_layouts_, {});
    }
    // D2D のビットマップは UI スレッドで解放させる。残りはどのスレッドで破棄してもよい。
    void ReleaseDiagrams() noexcept
    {
        diagrams_.clear();
    }

    // 未計測ノードの高さを推定した本文幅とズーム。幅 0 は幅を知らずに推定した (起動時の先読み)。
    struct EstimateBasis {
        float content_width = 0.0f;
        float zoom = 0.0f;
    };
    constexpr EstimateBasis GetEstimateBasis() const noexcept
    {
        return estimate_basis_;
    }
    constexpr void SetEstimateBasis(EstimateBasis basis) noexcept
    {
        estimate_basis_ = basis;
    }

private:
    // 全 text_layout を破棄する操作の後に呼ぶ。生存している範囲を空に戻し、以後に計測した範囲だけを追う。
    void ResetEvictionTracking() noexcept;

    // 計測で高さが変わるたびに後ろの全ノードをずらすと、数十万ノードの文書では 1 回で数百 µs かかる。
    // Top は 2^kTopBlockShift 件ごとのブロック内の値とブロックのオフセットに分けて持ち、ずらしを
    // 同じブロックの残りとブロック数ぶんの書き換えで済ませる。
    static constexpr size_t kTopBlockShift = 12;

    std::vector<NodeLayoutEntry> entries_;
    std::vector<float> tops_;
    std::vector<float> top_block_offsets_;
    std::unordered_map<size_t, DiagramEntry> diagrams_;
    ReleasedLayouts released_layouts_;
    bool has_dirty_nodes_ = true;
    EstimateBasis estimate_basis_;
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

// [from, size) のうち上端が y を超える最初のノード。該当なしの場合は cache.size() を返す。
constexpr size_t FindFirstNodeBelow(const LayoutCache& cache, float y, size_t from = 0) noexcept
{
    const auto rest = std::views::iota(from, cache.size());
    const auto it = std::ranges::partition_point(rest, [&cache, y](size_t i) noexcept {
        return cache.Top(i) <= y;
    });
    return it == rest.end() ? cache.size() : *it;
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
    return { first, FindFirstNodeBelow(cache, range_bottom, first) };
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
