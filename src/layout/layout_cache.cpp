#include "layout_cache.h"
#include "document_types.h"
#include <algorithm>
#include <ranges>

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

    if (table_row >= 0 && has_table_layout()) {
        const auto row = static_cast<size_t>(table_row);
        if (row < table_layout->row_heights.size() && row < table_layout->row_cum_y.size()) {
            base_y = entry_text_top + table_layout->row_cum_y[row];
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
        if (SUCCEEDED(layout->HitTestTextPosition(text_offset_w, FALSE, &px, &py, &htm))) {
            const float line_h = (htm.height > 0.0f) ? htm.height : fallback_h;
            return { base_y + py, line_h };
        }
    }
    return { base_y, fallback_h };
}

// ---------- LayoutCache private helpers ----------

void LayoutCache::ResetTableLayoutGeometry(TableLayoutData& tl) noexcept
{
    tl.cell_layouts.clear();
    tl.row_bgs_computed.clear();
    tl.row_links_applied.clear();
    tl.row_evicted.clear();
    tl.evicted_row_count = 0;
    tl.live_row_begin = 0;
    tl.live_row_end = 0;
    tl.natural_col_widths.clear();
    tl.cell_heights.clear();
    tl.cell_applied_widths.clear();
    tl.col_count = 0;
    tl.last_applied_max_width = -1.0f;
    tl.cached_table_width = 0.0f;
    tl.natural_total_width = 0.0f;
}

void LayoutCache::ResetEntryTextLayout(NodeLayoutEntry& e) noexcept
{
    e.text_layout.Reset();
    e.effects_applied = false;
    e.first_line_height = 0.0f;
    e.natural_code_width = 0.0f;
    e.clear_inline_code_bgs();
    e.invalidate_per_frame_hl_caches();
}

void LayoutCache::EvictEntryLayout(NodeLayoutEntry& e) noexcept
{
    // テーブルは列幅・行高さを残してセル layout だけ捨てる。幾何ごと捨てると再表示時に
    // 全セルの CreateTextLayout (巨大テーブルで秒単位) をやり直すことになる。
    if (e.table_layout && HasRowTracking(*e.table_layout)) {
        e.invalidate_per_frame_hl_caches();
        auto& tl = *e.table_layout;
        for (size_t r = tl.live_row_begin; r < tl.live_row_end; ++r) {
            EvictTableRow(tl, r);
        }
        tl.live_row_begin = 0;
        tl.live_row_end = 0;
        return;
    }
    if (!e.text_layout && !e.table_layout) {
        return;
    }
    ResetEntryTextLayout(e);
    e.table_layout.reset();
    e.layout_dirty = true;
}

bool LayoutCache::HasRowTracking(const TableLayoutData& tl) noexcept
{
    if (tl.cell_layouts.empty() || tl.col_count == 0 || tl.row_cum_y.empty()) {
        return false;
    }
    const size_t row_count = tl.row_cum_y.size() - 1;
    return row_count * tl.col_count == tl.cell_layouts.size() && tl.row_evicted.size() == row_count;
}

void LayoutCache::EvictTableRow(TableLayoutData& tl, size_t row_index) noexcept
{
    const size_t col_count = tl.col_count;
    if (col_count == 0) {
        return;
    }
    const size_t base = row_index * col_count;
    if (base + col_count > tl.cell_layouts.size()) {
        return;
    }
    for (size_t c = 0; c < col_count; ++c) {
        const size_t ci = base + c;
        if (tl.cell_layouts[ci]) {
            tl.cell_layouts[ci].Reset();
            tl.MarkRowEvicted(row_index);
        }
        if (ci < tl.cell_heights.size()) {
            tl.cell_heights[ci] = 0.0f;
        }
        if (ci < tl.cell_applied_widths.size()) {
            tl.cell_applied_widths[ci] = -1.0f;
        }
    }
}

// ---------- LayoutCache public ----------

void LayoutCache::Resize(size_t node_count)
{
    if (entries_.size() != node_count) {
        entries_.resize(node_count);
        diagrams_.resize(node_count);
        effects_generation_++;
        ResetEvictionTracking();
    }
}

void LayoutCache::Reset(size_t node_count, bool shrink)
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
    ResetEvictionTracking();
}

void LayoutCache::InvalidateAllLayouts() noexcept
{
    for (auto& e : entries_) {
        ResetEntryTextLayout(e);
        if (e.table_layout) {
            ResetTableLayoutGeometry(*e.table_layout);
            e.table_layout->cell_inline_code_bgs.clear();
        }
    }
    effects_generation_++;
    ResetEvictionTracking();
}

void LayoutCache::InvalidateEffectsAndDiagramBitmaps(const std::pmr::vector<Node>& nodes) noexcept
{
    for (auto& e : entries_) {
        e.effects_applied = false;
        e.clear_inline_code_bgs();
        if (e.table_layout) {
            e.table_layout->cell_inline_code_bgs.clear();
            e.table_layout->row_bgs_computed.clear();
            e.table_layout->row_links_applied.clear();
        }
    }
    effects_generation_++;
    InvalidateDiagramBitmaps(nodes);
}

void LayoutCache::InvalidateAllDiagramBitmaps() noexcept
{
    for (auto& d : diagrams_) {
        d.ResetForRetry();
    }
}

void LayoutCache::InvalidateDiagramBitmaps(const std::pmr::vector<Node>& nodes) noexcept
{
    const auto count = std::min(nodes.size(), diagrams_.size());
    for (const auto& [idx, node] : nodes | std::views::take(count) | std::views::enumerate) {
        if (IsDiagramCodeBlock(node)) {
            diagrams_[static_cast<size_t>(idx)].ResetForRetry();
        }
    }
}

void LayoutCache::EvictTextLayouts(size_t first_keep_inclusive, size_t last_keep_exclusive) noexcept
{
    const size_t n = entries_.size();
    const size_t fk = std::min(first_keep_inclusive, n);
    const size_t lk = std::min(last_keep_exclusive, n);

    if (last_evict_fk_ == 0 && last_evict_lk_ == 0) {
        for (size_t i = 0; i < fk; ++i) {
            EvictEntryLayout(entries_[i]);
        }
        for (size_t i = lk; i < n; ++i) {
            EvictEntryLayout(entries_[i]);
        }
    }
    else {
        // keep 範囲が縮小した差分のみ evict
        if (fk > last_evict_fk_) {
            for (size_t i = last_evict_fk_; i < fk; ++i) {
                EvictEntryLayout(entries_[i]);
            }
        }
        if (lk < last_evict_lk_) {
            for (size_t i = lk; i < last_evict_lk_; ++i) {
                EvictEntryLayout(entries_[i]);
            }
        }
    }
    last_evict_fk_ = fk;
    last_evict_lk_ = lk;
}

void LayoutCache::ResetEvictionTracking() noexcept
{
    last_evict_fk_ = 0;
    last_evict_lk_ = 0;
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
        auto& e = entries_[idx];
        if (!e.table_layout || !HasRowTracking(*e.table_layout)) {
            continue;
        }
        auto& tl = *e.table_layout;
        // 既に evict 済みの行はなめ直さず、生存範囲のうち keep 外の差分だけ捨てる。
        // 再可視化は dirty を経由せず LayoutEngine::EnsureVisibleLayout の行復元で行う。
        const auto [keep_begin, keep_end] = tl.VisibleRowRange(keep_top - e.text_top, keep_bottom - e.text_top);
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
}

void LayoutCache::NotifyDpiChanged() noexcept
{
    for (auto& e : entries_) {
        e.invalidate_per_frame_hl_caches();
    }
    effects_generation_++;
}
