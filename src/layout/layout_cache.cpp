#include "layout_cache.h"
#include "doc_dwrite_bridge.h"
#include "ui_constants.h"
#include <algorithm>
#include <cassert>
#include <ranges>

NodeLayoutEntry::Aux::~Aux() = default;

// ---------- TableLayoutData ----------

std::pair<size_t, size_t> TableLayoutData::VisibleRowRange(float local_top, float local_bottom) const noexcept
{
    size_t r_begin = 0;
    const auto upper = std::ranges::upper_bound(row_cum_y, local_top);
    if (upper != row_cum_y.begin()) {
        r_begin = static_cast<size_t>(std::ranges::distance(row_cum_y.begin(), upper)) - 1;
    }
    const auto lower = std::ranges::lower_bound(row_cum_y, local_bottom);
    return { r_begin, std::min(row_count, static_cast<size_t>(std::ranges::distance(row_cum_y.begin(), lower))) };
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
    float cx = TABLE_BORDER_WIDTH;
    for (size_t c = 0; c < col_count; c++) {
        col_cum_x[c] = cx;
        cx += ColumnStep(c);
    }
    col_cum_x[col_count] = cx;
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

} // namespace

void TableLayoutData::EvictRow(size_t r, ReleasedLayouts& released) noexcept
{
    bool buried = false;
    if (auto cells = std::move(row_layouts_[r])) {
        for (size_t c = 0; c < col_count; ++c) {
            if (cells[c]) {
                Bury(released, std::move(cells[c]));
                buried = true;
            }
        }
    }
    if (buried && !row_evicted_[r]) {
        row_evicted_[r] = 1;
        ++evicted_row_count_;
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
        tl->EvictRowsOutside(0, 0, released);
    }
    Bury(released, e.take_text_layout());
    e.release_layout_state();
    // 原文のまま組んだのは捨てた layout なので、組み直しを待つ印も下ろす (残すと数式が届くたびに再レイアウトが空振りする)。
    e.pending_inline = false;
}

void EvictEntryLayout(NodeLayoutEntry& e, ReleasedLayouts& released) noexcept
{
    // テーブルは列幅・行高さを残してセル layout だけ捨てる。幾何ごと捨てると再表示時に
    // 全セルの CreateTextLayout (巨大テーブルで秒単位) をやり直すことになる。
    if (auto* tl = e.table_layout()) {
        tl->EvictRowsOutside(0, 0, released);
        return;
    }
    // レイアウトを持たないノード (HR / 図 / 空テキスト) は計測済みのまま残す。
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
    assert(entries_.size() == nodes.size());
    for (size_t i = 0; i < entries_.size(); i++) {
        auto& e = entries_[i];
        // 表の幾何も旧倍率のものなので残さない (計測で全部作り直す)。
        ReleaseEntryLayouts(e, released_layouts_);
        // ズーム変更で計測条件が変わるので、測れなかったノードも改めて試す。旧倍率の実測値も推定に使わせない。
        e.layout_dirty = true;
        e.cached_width = NodeLayoutEntry::kUnmeasuredWidth;
        // 推定は高さを縮めないので、旧倍率の高さを残すとズームアウト後に総高さが過大なままになる。
        // 全レイアウトを捨てた直後は計測済みノードとの重なりが起きないので、推定からやり直させる。
        // 図のノードの高さは図の寸法で決まるので残す。
        if (nodes[i].type != NodeType::Diagram) {
            e.height = 0.0f;
        }
    }
    live_nodes_ = {};
    has_dirty_nodes_ = true;
    // 高さを 0 にした分の Y は、次の Reestimate に推定から組み直させる。
    estimate_basis_ = {};
}

bool LayoutCache::InvalidatePendingInlineLayouts(std::span<const size_t> table_indices, bool tables) noexcept
{
    bool any = false;
    const auto invalidate = [&](NodeLayoutEntry& e) {
        if (!e.pending_inline || (!tables && e.table_layout())) {
            return;
        }
        // 数式や画像の寸法で折り返しと行の高さが変わるので、表なら列幅と行の高さも含めて組み直す。
        ReleaseEntryLayouts(e, released_layouts_);
        e.layout_dirty = true;
        any = true;
    };
    // 印が立つのは組んだノードで、組んだ範囲 (live_nodes_) の外へ出たノードは evict で印を下ろす。表だけは evict が行を
    // 捨てるだけで印を残すので、範囲の外も見る。全エントリを見ると 30 万ノードで 1 回 130µs かかり、数式の組の結果や
    // 画像が届くたびに呼ばれる。
    for (size_t i = live_nodes_.begin; i < live_nodes_.end; ++i) {
        invalidate(entries_[i]);
    }
    if (tables) {
        for (const size_t i : table_indices) {
            if (i < live_nodes_.begin || i >= live_nodes_.end) {
                invalidate(entries_[i]);
            }
        }
    }
    has_dirty_nodes_ |= any;
    return any;
}

bool LayoutCache::InvalidateLayoutsOf(std::span<const size_t> indices) noexcept
{
    for (const size_t i : indices) {
        auto& e = entries_[i];
        ReleaseEntryLayouts(e, released_layouts_);
        e.layout_dirty = true;
    }
    has_dirty_nodes_ |= !indices.empty();
    return !indices.empty();
}

void LayoutCache::InvalidateAllDiagramCommands() noexcept
{
    for (auto& d : diagrams_ | std::views::values) {
        d.EvictForRetry();
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
    for (size_t k = 0; k < count; ++k) {
        entries_[to + k] = std::move(src.entries_[from + k]);
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
        NoteMaterialized(lo - from + to, hi - from + to);
    }
}

bool LayoutCache::TakeDiagramAsStale(LayoutCache& src, size_t from, size_t to)
{
    const auto it = src.diagrams_.find(from);
    if (it == src.diagrams_.end() || !it->second.HasImage()) {
        return false;
    }
    diagrams_.insert_or_assign(to, std::move(it->second)).first->second.MarkStale();
    entries_[to].height = src.entries_[from].height;
    return true;
}
