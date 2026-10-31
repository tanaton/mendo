#include "layout_cache.h"
#include "doc_dwrite_bridge.h"
#include "ui_constants.h"
#include <algorithm>
#include <cmath>
#include <optional>
#include <ranges>
#include <tuple>

namespace {

struct LineHit {
    uint32_t pos;
    float line_top;
    float line_bottom;
};

std::optional<LineHit> HitLineAt(IDWriteTextLayout* layout, float y) noexcept
{
    BOOL is_trailing = FALSE;
    BOOL is_inside = FALSE;
    DWRITE_HIT_TEST_METRICS m{};
    if (FAILED(layout->HitTestPoint(0.0f, y, &is_trailing, &is_inside, &m))) {
        return std::nullopt;
    }
    const auto line = LineBoxAt(layout, m.textPosition);
    if (!line) {
        return std::nullopt;
    }
    return LineHit{ m.textPosition, line->top, line->top + line->height };
}

// ローカル Y 範囲にかかる行のテキスト位置を、隣の行の位置で挟んだ [first, second) で返す。
// 行のテキスト範囲は論理順に並ぶので、隣の行ならどの x で引いても境界になる (RTL の行でも成り立つ)。
std::pair<uint32_t, uint32_t> TextRangeOfLines(IDWriteTextLayout* layout, const YRange& y) noexcept
{
    uint32_t begin = 0;
    uint32_t end = UINT32_MAX;
    if (std::isfinite(y.top)) {
        if (const auto first = HitLineAt(layout, y.top); first && first->line_top > 0.0f) {
            if (const auto prev = HitLineAt(layout, first->line_top - 0.5f); prev && prev->line_top < first->line_top) {
                begin = prev->pos;
            }
        }
    }
    if (std::isfinite(y.bottom)) {
        if (const auto last = HitLineAt(layout, y.bottom)) {
            if (const auto next = HitLineAt(layout, last->line_bottom + 0.5f); next && next->line_top > last->line_top) {
                end = next->pos;
            }
        }
    }
    return { begin, end };
}

} // namespace

const LineWindow& EnsureLineWindow(const NodeLayoutEntry& entry, YRange view)
{
    auto& window = entry.ensure_line_window();
    if (!window.y.Contains(view)) {
        // layout の端を越えた側は先に行が無いので、無限に伸ばしてその側の引き直しをなくす。
        constexpr float INF = std::numeric_limits<float>::infinity();
        const float margin = view.bottom - view.top;
        window.y = { view.top - margin, view.bottom + margin };
        if (window.y.top <= 0.0f) {
            window.y.top = -INF;
        }
        if (window.y.bottom >= entry.height) {
            window.y.bottom = INF;
        }
        std::tie(window.text_begin, window.text_end) = TextRangeOfLines(entry.text_layout(), window.y);
    }
    return window;
}

// ---------- TableLayoutData ----------

std::pair<size_t, size_t> TableLayoutData::VisibleRowRange(float local_top, float local_bottom) const noexcept
{
    size_t r_begin = 0;
    size_t r_end = row_count;
    const auto upper = std::ranges::upper_bound(row_cum_y, local_top);
    if (upper != row_cum_y.begin()) {
        r_begin = static_cast<size_t>(std::ranges::distance(row_cum_y.begin(), upper)) - 1;
    }
    const auto lower = std::ranges::lower_bound(row_cum_y, local_bottom);
    if (lower != row_cum_y.end()) {
        const auto idx = static_cast<size_t>(std::ranges::distance(row_cum_y.begin(), lower));
        r_end = std::min(row_count, idx);
    }
    return { r_begin, r_end };
}

std::pair<size_t, size_t> TableLayoutData::VisibleColRange(float local_left, float local_right) const noexcept
{
    // 列 c の右端は col_cum_x[c + 1]、左端は col_cum_x[c]。
    const auto rights = std::ranges::subrange(col_cum_x.begin() + 1, col_cum_x.end());
    const auto c_begin = static_cast<size_t>(std::ranges::lower_bound(rights, local_left) - rights.begin());
    const auto lefts = std::ranges::subrange(col_cum_x.begin() + static_cast<ptrdiff_t>(c_begin), col_cum_x.end() - 1);
    const auto c_end = static_cast<size_t>(std::ranges::upper_bound(lefts, local_right) - col_cum_x.begin());
    return { c_begin, c_end };
}

void TableLayoutData::RebuildRowCumY(size_t first_row)
{
    if (row_cum_y.empty()) {
        first_row = 0;
        row_cum_y.resize(row_count + 1);
    }
    // row_cum_y[0] は常に 0。加算順は全行の積み直しと同じなので、途中から再開しても結果は一致する。
    float ry = row_cum_y[first_row];
    for (size_t r = first_row; r < row_count; r++) {
        row_cum_y[r] = ry;
        ry += row_heights[r] + TABLE_BORDER_WIDTH;
    }
    row_cum_y[row_count] = ry;
}

void TableLayoutData::RebuildColCumX()
{
    const size_t n = col_widths.size();
    col_cum_x.resize(n + 1);
    float cx = TABLE_BORDER_WIDTH;
    for (size_t c = 0; c < n; c++) {
        col_cum_x[c] = cx;
        cx += ColumnStep(c);
    }
    col_cum_x[n] = cx;
}

int TableLayoutData::RowIndexAt(float local_y) const noexcept
{
    const auto it = std::ranges::upper_bound(row_cum_y, local_y);
    if (it == row_cum_y.begin()) {
        return -1;
    }
    const auto idx = std::ranges::distance(row_cum_y.begin(), it) - 1;
    if (static_cast<size_t>(idx) >= row_count) {
        return -1;
    }
    return static_cast<int>(idx);
}

// ---------- NodeLayoutEntry ----------

std::pair<float, float> NodeLayoutEntry::GetMatchYRange(
    int table_row, int table_col, uint32_t text_offset_w, float entry_text_top) const noexcept
{
    IDWriteTextLayout* layout = nullptr;
    float base_y = entry_text_top;
    float fallback_h = height;

    if (const auto* tl = table_layout(); table_row >= 0 && tl) {
        const auto row = static_cast<size_t>(table_row);
        if (row < tl->row_count) {
            base_y = entry_text_top + tl->row_cum_y[row];
            fallback_h = tl->row_heights[row];
            if (table_col >= 0 && static_cast<size_t>(table_col) < tl->col_count) {
                layout = tl->GetCellLayout(row, static_cast<size_t>(table_col));
            }
        }
        if (layout != nullptr) {
            base_y = TableCellTextOrigin(0.0f, base_y).y;
        }
    }
    else {
        layout = text_layout();
    }

    if (layout != nullptr) {
        if (const auto line = LineBoxAt(layout, text_offset_w)) {
            const float line_h = (line->height > 0.0f) ? line->height : fallback_h;
            return { base_y + line->top, line_h };
        }
    }
    return { base_y, fallback_h };
}

// ---------- LayoutCache helpers ----------

namespace {

// 確保に失敗したらその場で解放する。
void Bury(ReleasedLayouts& released, Microsoft::WRL::ComPtr<IDWriteTextLayout>&& layout) noexcept
{
    if (!layout) {
        return;
    }
    try {
        released.push_back(std::move(layout));
    } catch (const std::bad_alloc&) {
        layout.Reset();
    }
}

void BuryCells(TableLayoutData& tl, ReleasedLayouts& released) noexcept
{
    for (auto& cell : tl.cell_layouts) {
        Bury(released, std::move(cell));
    }
}

} // namespace

void TableLayoutData::EvictRow(size_t r, ReleasedLayouts& released) noexcept
{
    const size_t base = CellIndex(r, 0);
    for (size_t ci = base; ci < base + col_count; ++ci) {
        if (cell_layouts[ci]) {
            Bury(released, std::move(cell_layouts[ci]));
            if (!row_evicted_[r]) {
                row_evicted_[r] = 1;
                ++evicted_row_count_;
            }
        }
    }
}

void TableLayoutData::EvictRowsOutside(size_t keep_begin, size_t keep_end, ReleasedLayouts& released) noexcept
{
    live_rows_.ShrinkTo(keep_begin, keep_end, [&](size_t r) noexcept { EvictRow(r, released); });
}

namespace {

// テキストとセルのレイアウトを released へ移し、計測・描画で作った状態をテーブル幾何も含めて捨てる。
void ReleaseEntryLayouts(NodeLayoutEntry& e, ReleasedLayouts& released) noexcept
{
    if (auto* tl = e.table_layout()) {
        BuryCells(*tl, released);
    }
    Bury(released, e.take_text_layout());
    e.release_layout_state();
    // 原文のまま組んだのは捨てた layout なので、組み直しを待つ印も下ろす (残すと数式が届くたびに再レイアウトが空振りする)。
    e.pending_math = false;
}

void EvictEntryLayout(NodeLayoutEntry& e, ReleasedLayouts& released) noexcept
{
    // テーブルは列幅・行高さを残してセル layout だけ捨てる。幾何ごと捨てると再表示時に
    // 全セルの CreateTextLayout (巨大テーブルで秒単位) をやり直すことになる。
    if (auto* tl = e.table_layout()) {
        e.invalidate_per_frame_hl_caches();
        tl->EvictRowsOutside(0, 0, released);
        return;
    }
    // レイアウトを持たないノード (HR / 画像 / 空テキスト) は計測済みのまま残す。
    const bool had_layout = e.text_layout() != nullptr;
    ReleaseEntryLayouts(e, released);
    if (had_layout) {
        e.layout_dirty = true;
    }
}

} // namespace

// ---------- LayoutCache public ----------

void LayoutCache::InvalidateAllLayouts(const std::vector<Node>& nodes) noexcept
{
    for (size_t i = 0; i < entries_.size(); i++) {
        auto& e = entries_[i];
        // 表の幾何も旧倍率のものなので残さない (計測で全部作り直す)。
        ReleaseEntryLayouts(e, released_layouts_);
        // ズーム変更で計測条件が変わるので、測れなかったノードも改めて試す。旧倍率の実測値も推定に使わせない。
        e.layout_dirty = true;
        e.cached_width = NodeLayoutEntry::kUnmeasuredWidth;
        // 推定は高さを縮めないので、旧倍率の高さを残すとズームアウト後に総高さが過大なままになる。
        // 全レイアウトを捨てた直後は計測済みノードとの重なりが起きないので、推定からやり直させる。
        // ビットマップを描くノードの高さはビットマップ側で決まるので残す。
        if (i < nodes.size() && !IsBitmapDrawNode(nodes[i])) {
            e.height = 0.0f;
        }
    }
    live_nodes_ = {};
    has_dirty_nodes_ = true;
    // 高さを 0 にした分の Y は、次の Reestimate に推定から組み直させる。
    estimate_basis_ = {};
}

bool LayoutCache::InvalidatePendingMathLayouts(bool tables) noexcept
{
    bool any = false;
    for (auto& e : entries_) {
        if (!e.pending_math && !(tables && e.table_layout() && e.table_layout()->pending_math)) {
            continue;
        }
        // 数式の寸法で折り返しと行の高さが変わるので、表なら列幅と行の高さも含めて組み直す。
        ReleaseEntryLayouts(e, released_layouts_);
        e.layout_dirty = true;
        any = true;
    }
    has_dirty_nodes_ |= any;
    return any;
}

void LayoutCache::InvalidateAllDiagramBitmaps() noexcept
{
    for (auto& d : diagrams_ | std::views::values) {
        d.EvictForRetry();
    }
}

void LayoutCache::InvalidateDiagramBitmaps(std::span<const size_t> diagram_indices) noexcept
{
    for (const size_t idx : diagram_indices) {
        if (auto* d = FindDiagram(idx)) {
            d->MarkStale();
        }
    }
}

void LayoutCache::EvictDiagramBitmapsOutside(size_t first_keep_inclusive, size_t last_keep_exclusive) noexcept
{
    for (auto& [idx, d] : diagrams_) {
        if (idx < first_keep_inclusive || idx >= last_keep_exclusive) {
            d.EvictBitmap();
        }
    }
}

void LayoutCache::EvictTextLayouts(size_t first_keep_inclusive, size_t last_keep_exclusive) noexcept
{
    const size_t n = entries_.size();
    // layout を作る経路 (MeasureIndicesParallel・表の行の復元・TakeEntries) はどれも NoteMaterialized を通るので、
    // 記録した範囲の keep 外だけ走査すればよい (30 万ノードの文書を開いた直後の 1 回目が 1.2ms → 0)。
    live_nodes_.ShrinkTo(std::min(first_keep_inclusive, n), std::min(last_keep_exclusive, n), [&](size_t i) noexcept {
        EvictEntryLayout(entries_[i], released_layouts_);
    });
}

void LayoutCache::EvictInvisibleTableRows(std::span<const size_t> table_indices, YRange keep) noexcept
{
    for (const size_t idx : table_indices) {
        if (idx >= entries_.size()) {
            continue;
        }
        auto* tl = entries_[idx].table_layout();
        if (!tl) {
            continue;
        }
        // 再可視化は dirty を経由せず LayoutEngine::MeasurePass の行復元で行う。
        const auto local = keep.ToLocal(Top(idx));
        const auto [keep_begin, keep_end] = tl->VisibleRowRange(local.top, local.bottom);
        tl->EvictRowsOutside(keep_begin, keep_end, released_layouts_);
    }
}

void LayoutCache::TakeEntries(LayoutCache& src, size_t from, size_t to, size_t count)
{
    if (count == 0) {
        return;
    }
    for (size_t k = 0; k < count; ++k) {
        auto& entry = entries_[to + k] = std::move(src.entries_[from + k]);
        entry.invalidate_per_frame_hl_caches();
    }
    for (auto& [index, diagram] : src.diagrams_) {
        if (index >= from && index - from < count) {
            diagrams_.insert_or_assign(index - from + to, std::move(diagram));
        }
    }
    // 次の evict で keep 範囲外に移った分も拾わせる。layout を持ちうるのは src の生存範囲だけなので、移した範囲と
    // 重なる分だけ記録する。丸ごと記録すると保存のたびに次の evict が全エントリをなめる
    // (30 万ノードの文書で 1.2ms、BM Reload/FirstEvictAfterCarryOver)。
    const size_t lo = std::max(src.live_nodes_.begin, from);
    const size_t hi = std::min(src.live_nodes_.end, from + count);
    if (lo < hi) {
        NoteMaterialized(lo - from + to, hi - 1 - from + to);
    }
}

bool LayoutCache::TakeDiagramAsStale(LayoutCache& src, size_t from, size_t to)
{
    const auto it = src.diagrams_.find(from);
    if (it == src.diagrams_.end() || !it->second.bitmap) {
        return false;
    }
    auto& d = diagrams_.insert_or_assign(to, std::move(it->second)).first->second;
    d.MarkStale();
    // コードのハッシュは新しいノードのテキストで計算し直させる。
    d.code_hash = 0;
    d.png.reset();
    entries_[to].height = src.entries_[from].height;
    return true;
}
