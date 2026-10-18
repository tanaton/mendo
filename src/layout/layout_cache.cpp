#include "layout_cache.h"
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
    return LineHit{ m.textPosition, m.top, m.top + m.height };
}

// ローカル Y 範囲にかかる行のテキスト位置を、隣の行の位置で挟んだ [first, second) で返す。
// 行のテキスト範囲は論理順に並ぶので、隣の行ならどの x で引いても境界になる (RTL の行でも成り立つ)。
std::pair<uint32_t, uint32_t> TextRangeOfLines(IDWriteTextLayout* layout, const LocalYRange& y) noexcept
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

const LineWindow& EnsureLineWindow(const NodeLayoutEntry& entry, LocalYRange view)
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
    if (row_cum_y.empty()) {
        return { 0, 0 };
    }
    const size_t row_count = row_cum_y.size() - 1;
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

void TableLayoutData::RebuildRowCumY(size_t row_count, size_t first_row)
{
    if (row_cum_y.size() != row_count + 1) {
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
    if (row_cum_y.empty()) {
        return -1;
    }
    const size_t row_count = row_cum_y.size() - 1;
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
        if (row < tl->row_heights.size() && row < tl->row_cum_y.size()) {
            base_y = entry_text_top + tl->row_cum_y[row];
            fallback_h = tl->row_heights[row];
        }
        if (table_col >= 0) {
            layout = tl->GetCellLayout(row, static_cast<size_t>(table_col));
        }
        if (layout != nullptr) {
            // セル本文は行上端から TABLE_CELL_PADDING 下に描かれる (GenTable の text_y と同じ)。
            base_y += TABLE_CELL_PADDING;
        }
    }
    else {
        layout = text_layout();
    }

    if (layout != nullptr) {
        FLOAT px = 0.0f, py = 0.0f;
        DWRITE_HIT_TEST_METRICS htm{};
        if (SUCCEEDED(layout->HitTestTextPosition(text_offset_w, FALSE, &px, &py, &htm))) {
            const float line_h = (htm.height > 0.0f) ? htm.height : fallback_h;
            return { base_y + py, line_h };
        }
    }
    return { base_y, fallback_h };
}

// ---------- LayoutCache helpers ----------

namespace {

// 行単位 evict できる状態 (幾何と管理配列が揃っている) か。管理配列は MeasureTable が確保する。
bool HasRowTracking(const TableLayoutData& tl) noexcept
{
    if (tl.cell_layouts.empty() || tl.col_count == 0 || tl.row_cum_y.empty()) {
        return false;
    }
    const size_t row_count = tl.row_cum_y.size() - 1;
    return row_count * tl.col_count == tl.cell_layouts.size() && tl.row_evicted.size() == row_count;
}

// 1 行分の cell_layouts を Reset し、cell_heights / cell_applied_widths を再計測待ちに戻す。
// 行/列の幾何 (row_cum_y, col_cum_x, row_heights, col_widths) は維持する。
// HasRowTracking を満たすテーブルの row_index < 行数 でのみ呼ぶ (セル単位の配列は AllocateCells が揃えている)。
void EvictTableRow(TableLayoutData& tl, size_t row_index) noexcept
{
    const size_t base = tl.CellIndex(row_index, 0);
    for (size_t ci = base; ci < base + tl.col_count; ++ci) {
        if (tl.cell_layouts[ci]) {
            tl.cell_layouts[ci].Reset();
            tl.MarkRowEvicted(row_index);
        }
        tl.cell_heights[ci] = 0.0f;
        tl.cell_applied_widths[ci] = -1.0f;
    }
}

// 生存範囲のうち [keep_begin, keep_end) の外の行を evict し、生存範囲を keep との重なりに縮める。
// evict 済みの行はなめ直さない。HasRowTracking を満たすテーブルでのみ呼ぶ。
void EvictTableRowsOutside(TableLayoutData& tl, size_t keep_begin, size_t keep_end) noexcept
{
    const size_t live_begin = tl.live_row_begin;
    const size_t live_end = tl.live_row_end;
    for (size_t r = live_begin; r < std::min(live_end, keep_begin); ++r) {
        EvictTableRow(tl, r);
    }
    for (size_t r = std::max(live_begin, keep_end); r < live_end; ++r) {
        EvictTableRow(tl, r);
    }
    tl.live_row_begin = std::max(live_begin, keep_begin);
    tl.live_row_end = std::min(live_end, keep_end);
    if (tl.live_row_begin >= tl.live_row_end) {
        tl.live_row_begin = 0;
        tl.live_row_end = 0;
    }
}

void EvictEntryLayout(NodeLayoutEntry& e) noexcept
{
    // テーブルは列幅・行高さを残してセル layout だけ捨てる。幾何ごと捨てると再表示時に
    // 全セルの CreateTextLayout (巨大テーブルで秒単位) をやり直すことになる。
    if (auto* tl = e.table_layout(); tl && HasRowTracking(*tl)) {
        e.invalidate_per_frame_hl_caches();
        EvictTableRowsOutside(*tl, 0, 0);
        return;
    }
    // レイアウトを持たないノード (HR / 画像 / 空テキスト) は計測済みのまま残す。
    const bool had_layout = e.text_layout() || e.table_layout();
    e.release_layout_state();
    if (had_layout) {
        e.layout_dirty = true;
    }
}

// セルのレイアウトと計測値、列幅の適用状態を捨てる。行/列の幾何は次の計測で作り直される。
void ResetTableCellCaches(TableLayoutData& tl) noexcept
{
    tl.row_code_bgs.clear();
    tl.cell_layouts.clear();
    tl.ResetRowEviction(0);
    tl.natural_col_widths.clear();
    tl.cell_heights.clear();
    tl.cell_applied_widths.clear();
    tl.col_count = 0;
    tl.last_applied_max_width = -1.0f;
    tl.cached_table_width = 0.0f;
    tl.natural_total_width = 0.0f;
}

} // namespace

// ---------- LayoutCache public ----------

void LayoutCache::Resize(size_t node_count)
{
    if (entries_.size() != node_count) {
        entries_.resize(node_count);
        tops_.resize(node_count);
        std::erase_if(diagrams_, [node_count](const auto& kv) { return kv.first >= node_count; });
        ResetEvictionTracking();
        has_dirty_nodes_ = true;
    }
}

void LayoutCache::Reset(size_t node_count)
{
    const auto reset = [node_count](auto& v) {
        v.clear();
        v.resize(node_count);
    };
    reset(entries_);
    reset(tops_);
    diagrams_ = {};
    ResetEvictionTracking();
    has_dirty_nodes_ = true;
}

void LayoutCache::InvalidateAllLayouts(const std::pmr::vector<Node>& nodes) noexcept
{
    for (size_t i = 0; i < entries_.size(); i++) {
        auto& e = entries_[i];
        e.reset_layout_state();
        // ズーム変更で計測条件が変わるので、諦めたノードも改めて試す。旧倍率の実測値も推定に使わせない。
        e.measure_failures = 0;
        e.cached_width = NodeLayoutEntry::kUnmeasuredWidth;
        e.cached_height = 0.0f;
        // 推定は高さを縮めないので、旧倍率の高さを残すとズームアウト後に総高さが過大なままになる。
        // 全レイアウトを捨てた直後は計測済みノードとの重なりが起きないので、推定からやり直させる。
        // ビットマップを描くノードの高さはビットマップ側で決まるので残す。
        if (i < nodes.size() && !IsBitmapDrawNode(nodes[i])) {
            e.height = 0.0f;
        }
        if (auto* tl = e.table_layout()) {
            ResetTableCellCaches(*tl);
        }
    }
    ResetEvictionTracking();
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
            d->ResetForRetry();
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
    const size_t fk = std::min(first_keep_inclusive, n);
    const size_t lk = std::min(last_keep_exclusive, n);

    const auto evict_outside_keep = [&](size_t begin, size_t end) noexcept {
        for (size_t i = begin; i < std::min(end, fk); ++i) {
            EvictEntryLayout(entries_[i]);
        }
        for (size_t i = std::max(begin, lk); i < end; ++i) {
            EvictEntryLayout(entries_[i]);
        }
    };
    // layout が生存しうるのは前回 keep 範囲と、その後に計測した範囲だけなので、その keep 外だけ走査する。
    // 未追跡 (0, 0) は前回 keep を全域 [0, n) とみなす。
    const size_t prev_lk = (last_evict_fk_ == 0 && last_evict_lk_ == 0) ? n : std::min(last_evict_lk_, n);
    evict_outside_keep(std::min(last_evict_fk_, n), prev_lk);
    evict_outside_keep(std::min(materialized_begin_, n), std::min(materialized_end_, n));
    last_evict_fk_ = fk;
    last_evict_lk_ = lk;
    materialized_begin_ = SIZE_MAX;
    materialized_end_ = 0;
}

void LayoutCache::ResetEvictionTracking() noexcept
{
    last_evict_fk_ = 0;
    last_evict_lk_ = 0;
    materialized_begin_ = SIZE_MAX;
    materialized_end_ = 0;
}

void LayoutCache::EvictInvisibleTableRows(
    std::span<const size_t> table_indices,
    float viewport_top, float viewport_bottom, float buffer_screens_height) noexcept
{
    const float keep_top = viewport_top - buffer_screens_height;
    const float keep_bottom = viewport_bottom + buffer_screens_height;
    for (const size_t idx : table_indices) {
        if (idx >= entries_.size()) {
            continue;
        }
        auto* tl = entries_[idx].table_layout();
        if (!tl || !HasRowTracking(*tl)) {
            continue;
        }
        // 再可視化は dirty を経由せず LayoutEngine::EnsureVisibleLayout の行復元で行う。
        const auto [keep_begin, keep_end] = tl->VisibleRowRange(keep_top - tops_[idx], keep_bottom - tops_[idx]);
        EvictTableRowsOutside(*tl, keep_begin, keep_end);
    }
}

void LayoutCache::NotifyDpiChanged() noexcept
{
    for (auto& e : entries_) {
        e.invalidate_per_frame_hl_caches();
    }
}

void LayoutCache::TakeEntry(LayoutCache& src, size_t from, size_t to)
{
    auto& entry = entries_[to];
    entry = std::move(src.entries_[from]);
    entry.invalidate_per_frame_hl_caches();
    if (const auto it = src.diagrams_.find(from); it != src.diagrams_.end()) {
        diagrams_.insert_or_assign(to, std::move(it->second));
    }
    // 次の evict で keep 範囲外に移った分も拾わせる。
    NoteMaterialized(to);
}
