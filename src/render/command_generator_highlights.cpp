#include "command_generator_internal.h"
#include "profiler.h"
#include <algorithm>
#include <cassert>
#include <ranges>
#include <utility>

std::span<const DWRITE_HIT_TEST_METRICS> CommandGenerator::HitTestRange(IDWriteTextLayout* layout, uint32_t start, uint32_t length)
{
    assert(hit_test_buffer_ && "SetHitTestBuffer must be called before GenerateMdPane");
    MENDO_COUNT_INC(g_cmd_gen_stats.hittest_range);
    const UINT32 count = FetchHitTestMetrics(layout, start, length, *hit_test_buffer_);
    return { hit_test_buffer_->data(), count };
}

void CommandGenerator::EmitFillRectIfVisible(const D2D1_RECT_F& r, D2D1_COLOR_F color, BrushId brush)
{
    if (OverlapsY(r, cull_top_, cull_bottom_)) {
        cmds_.emplace_back(FillRectCmd{ r, color, brush });
    }
}

void CommandGenerator::GenSelectionHighlight(IDWriteTextLayout* layout, uint32_t start, uint32_t length, float origin_x, float origin_y)
{
    if (!layout || length == 0) {
        return;
    }
    for (const auto& m : HitTestRange(layout, start, length)) {
        EmitFillRectIfVisible(RectFromHitTest(m, origin_x, origin_y), SELECTION_COLOR, BrushId::Selection);
    }
}

void CommandGenerator::CollectHitTestRects(IDWriteTextLayout* layout, uint32_t start, uint32_t length, std::pmr::vector<D2D1_RECT_F>& out)
{
    // 少しずつ追記されるので reserve はしない (要求ぴったりの再確保が毎回走る)。
    for (const auto& m : HitTestRange(layout, start, length)) {
        out.emplace_back(RectFromHitTest(m));
    }
}

const LineWindow& CommandGenerator::LineWindowAround(const NodeLayoutEntry& entry, float origin_y) const
{
    return EnsureLineWindow(entry, { cull_top_ - origin_y, cull_bottom_ - origin_y });
}

void CommandGenerator::GenSelectionHighlightCached(const Node& node, const NodeLayoutEntry& entry, uint32_t doc_start, uint32_t doc_length, float origin_x, float origin_y)
{
    auto* layout = entry.text_layout();
    if (!layout || doc_length == 0) {
        return;
    }
    auto& cache = entry.ensure_selection_hl_cache();
    // キャッシュキーは UTF-8 byte 単位 (selection 状態と直接対応)。
    // 位置変換は選択が変わったときだけ前方走査で行う。
    if (cache.layout_ptr != layout || cache.start != doc_start || cache.length != doc_length) {
        mendo::Utf16OffsetCursor cursor{ node.GetText() };
        const auto wr = cursor.WideRange(doc_start, doc_length);
        if (cache.layout_ptr != layout) {
            cache.rects.clear();
            cache.rects_begin_w = 0;
            cache.rects_end_w = 0;
        }
        cache.layout_ptr = layout;
        cache.start = doc_start;
        cache.length = doc_length;
        cache.start_w = wr.startPosition;
        cache.end_w = wr.startPosition + wr.length;
    }
    // 背の高いノードでは選択範囲を可視範囲の前後の行に絞って引き、全行選択でも矩形を可視行数ぶんで済ませる。
    const auto& window = LineWindowAround(entry, origin_y);
    uint32_t begin = std::max(cache.start_w, window.text_begin);
    uint32_t end = std::min(cache.end_w, window.text_end);
    if (end <= begin) {
        begin = end = 0;
    }
    if (begin != cache.rects_begin_w || end != cache.rects_end_w) {
        MENDO_COUNT_INC(g_cmd_gen_stats.sel_hl_cache_miss);
        cache.rects.clear();
        if (end > begin) {
            CollectHitTestRects(layout, begin, end - begin, cache.rects);
        }
        cache.rects_begin_w = begin;
        cache.rects_end_w = end;
    }
    else {
        MENDO_COUNT_INC(g_cmd_gen_stats.sel_hl_cache_hit);
    }
    for (const auto& local : cache.rects) {
        EmitFillRectIfVisible(OffsetRectF(local, origin_x, origin_y), SELECTION_COLOR, BrushId::Selection);
    }
}

CommandGenerator::NodeSearchMatches CommandGenerator::PrepareNodeSearchMatches(const NodeLayoutEntry& entry, int node_index)
{
    if (!search_matches_ || search_matches_->empty()) {
        return {};
    }
    const std::span<const SearchMatch> matches = *search_matches_;
    const auto range = std::ranges::equal_range(matches, node_index, {}, &SearchMatch::node_index);
    if (range.empty()) {
        return {};
    }
    const size_t first_global = static_cast<size_t>(range.begin() - matches.begin());

    // layout 変更時は invalidate_search_hl_cache() でキャッシュ自体が破棄されており、SearchState の
    // generation は 1 から始まるため、cache.gen == search_generation_ のみでキャッシュ有効性を判定できる。
    auto& cache = entry.ensure_search_hl_cache();
    if (cache.gen != search_generation_) {
        MENDO_COUNT_INC(g_cmd_gen_stats.search_hl_reset);
        cache.rects.clear();
        cache.spans.assign(range.size(), {});
        cache.gen = search_generation_;
    }
    return { matches.subspan(first_global, range.size()), first_global, &cache };
}

void CommandGenerator::EmitSearchMatches(const NodeSearchMatches& nm, IDWriteTextLayout* layout, size_t begin, size_t end, float origin_x, float origin_y)
{
    auto& cache = *nm.cache;
    for (size_t i = begin; i < end; ++i) {
        auto& span = cache.spans[i];
        if (span.count == SearchHlCache::Span::NOT_COMPUTED) {
            // evict 中のセルは未計算のまま残し、layout が復元されてから計算する。
            if (!layout) {
                MENDO_COUNT_INC(g_cmd_gen_stats.search_hl_provisional);
                continue;
            }
            MENDO_COUNT_INC(g_cmd_gen_stats.search_hl_computed);
            const auto& m = nm.matches[i];
            span.begin = static_cast<uint32_t>(cache.rects.size());
            if (m.length_w > 0) {
                CollectHitTestRects(layout, m.start_w, m.length_w, cache.rects);
            }
            span.count = static_cast<uint32_t>(cache.rects.size()) - span.begin;
        }

        const bool is_current = (static_cast<int>(nm.first_global + i) == current_match_index_);
        const D2D1_COLOR_F color = is_current ? theme_->search_highlight_current_color : theme_->search_highlight_color;
        const BrushId hl_brush = is_current ? BrushId::SearchHighlightCurrent : BrushId::SearchHighlight;
        for (const auto& r : std::span{ cache.rects }.subspan(span.begin, span.count)) {
            EmitFillRectIfVisible(OffsetRectF(r, origin_x, origin_y), color, hl_brush);
        }
    }
}

void CommandGenerator::GenSearchHighlights(const NodeLayoutEntry& entry, int node_index, float origin_x, float origin_y)
{
    const auto nm = PrepareNodeSearchMatches(entry, node_index);
    if (nm.matches.empty()) {
        return;
    }
    // マッチは start_w 昇順で重ならないので、終端も昇順に並ぶ。
    const auto& window = LineWindowAround(entry, origin_y);
    const auto first = std::ranges::partition_point(nm.matches, [b = window.text_begin](const SearchMatch& m) noexcept {
        return m.start_w + m.length_w <= b;
    });
    const auto last = std::ranges::partition_point(first, nm.matches.end(), [e = window.text_end](const SearchMatch& m) noexcept {
        return m.start_w < e;
    });
    EmitSearchMatches(nm, entry.text_layout(),
        static_cast<size_t>(first - nm.matches.begin()), static_cast<size_t>(last - nm.matches.begin()), origin_x, origin_y);
}

void CommandGenerator::GenCellSearchHighlights(const NodeLayoutEntry& entry, int node_index, size_t row, size_t col, float origin_x, float origin_y)
{
    const auto nm = PrepareNodeSearchMatches(entry, node_index);
    if (nm.matches.empty()) {
        return;
    }
    // テーブル内 match は (row, col, start) 昇順なので、セル範囲を二分探索で切り出す。
    const auto cell = std::ranges::equal_range(
        nm.matches, std::pair{ static_cast<int>(row), static_cast<int>(col) }, {},
        [](const SearchMatch& m) noexcept { return std::pair{ m.table_row, m.table_col }; });
    EmitSearchMatches(nm, entry.table_layout()->GetCellLayout(row, col),
        static_cast<size_t>(cell.begin() - nm.matches.begin()), static_cast<size_t>(cell.end() - nm.matches.begin()), origin_x, origin_y);
}
