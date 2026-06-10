#include "command_generator.h"
#include "command_generator_internal.h"
#include "profiler.h"
#include <cassert>
#include <ranges>

// DWRITE_HIT_TEST_METRICS を origin 加算付きの D2D1_RECT_F に変換する。
static inline D2D1_RECT_F RectFromHitTest(const DWRITE_HIT_TEST_METRICS& m, float origin_x = 0.0f, float origin_y = 0.0f) noexcept
{
    return D2D1::RectF(
        origin_x + m.left,
        origin_y + m.top,
        origin_x + m.left + m.width,
        origin_y + m.top + m.height);
}

void CommandGenerator::EmitHighlightRects(
    DrawCommandList& cmds,
    IDWriteTextLayout* layout,
    uint32_t start,
    uint32_t length,
    float origin_x,
    float origin_y,
    D2D1_COLOR_F color,
    BrushId brush_id)
{
    if (!layout || length == 0) {
        return;
    }
    assert(hit_test_buffer_ && "SetHitTestBuffer must be called before GenerateMdPane");
    auto& buf = *hit_test_buffer_;
    MENDO_COUNT_INC(g_cmd_gen_stats.hittest_range);
    const UINT32 count = FetchHitTestMetrics(layout, start, length, buf);
    for (UINT32 i = 0; i < count; i++) {
        cmds.emplace_back(FillRectCmd{ RectFromHitTest(buf[i], origin_x, origin_y), color, brush_id });
    }
}

void CommandGenerator::GenSelectionHighlight(DrawCommandList& cmds, IDWriteTextLayout* layout, uint32_t start, uint32_t length, float origin_x, float origin_y)
{
    EmitHighlightRects(cmds, layout, start, length, origin_x, origin_y, SELECTION_COLOR, BrushId::Selection);
}

void CommandGenerator::CollectHitTestRects(IDWriteTextLayout* layout, uint32_t start, uint32_t length, std::pmr::vector<D2D1_RECT_F>& out)
{
    assert(hit_test_buffer_ && "SetHitTestBuffer must be called before GenerateMdPane");
    auto& buf = *hit_test_buffer_;
    MENDO_COUNT_INC(g_cmd_gen_stats.hittest_range);
    const UINT32 count = FetchHitTestMetrics(layout, start, length, buf);
    out.reserve(out.size() + count);
    for (UINT32 i = 0; i < count; i++) {
        out.emplace_back(RectFromHitTest(buf[i]));
    }
}

void CommandGenerator::GenSelectionHighlightCached(DrawCommandList& cmds, const Node& node, const NodeLayoutEntry& entry, uint32_t doc_start, uint32_t doc_length, float origin_x, float origin_y)
{
    auto* layout = entry.text_layout.Get();
    if (!layout || doc_length == 0) {
        return;
    }
    auto& cache = entry.ensure_selection_hl_cache();
    // キャッシュキーは UTF-8 byte 単位 (selection 状態と直接対応)。
    // miss 時は WideViewCache 経由で同一ノードの連続 miss でも decode を 1 回に抑える。
    if (cache.layout_ptr != layout || cache.start != doc_start || cache.length != doc_length) {
        MENDO_COUNT_INC(g_cmd_gen_stats.sel_hl_cache_miss);
        cache.rects.clear();
        const auto wr = node_wv_.WideRange(node.GetText(), doc_start, doc_length);
        if (wr.length > 0) {
            CollectHitTestRects(layout, wr.startPosition, wr.length, cache.rects);
        }
        cache.layout_ptr = layout;
        cache.start = doc_start;
        cache.length = doc_length;
    }
    else {
        MENDO_COUNT_INC(g_cmd_gen_stats.sel_hl_cache_hit);
    }
    for (const auto& r : cache.rects) {
        cmds.emplace_back(FillRectCmd{ OffsetRectF(r, origin_x, origin_y), SELECTION_COLOR, BrushId::Selection });
    }
}

void CommandGenerator::GenSearchHighlights(DrawCommandList& cmds, const NodeLayoutEntry& entry, int node_index, float origin_x, float origin_y, int table_row, int table_col)
{
    if (!search_matches_ || search_matches_->empty()) {
        return;
    }

    const std::span<const SearchMatch> matches = *search_matches_;
    const auto range = std::ranges::equal_range(matches, node_index, {}, &SearchMatch::node_index);
    if (range.empty()) {
        return;
    }
    const size_t first_global = static_cast<size_t>(range.begin() - matches.begin());
    const size_t node_match_count = static_cast<size_t>(range.size());

    auto& cache = entry.ensure_search_hl_cache();
    RebuildSearchHlCache(cache, entry, matches, first_global, node_match_count);
    EmitSearchHlCommands(cmds, cache, matches, first_global, origin_x, origin_y, table_row, table_col);
}

void CommandGenerator::RebuildSearchHlCache(
    SearchHlCache& cache, const NodeLayoutEntry& entry,
    std::span<const SearchMatch> matches, size_t first_global, size_t node_match_count)
{
    // キャッシュミス時のみ HitTestTextRange を一括発行。layout 変更時は
    // invalidate_search_hl_cache() でキャッシュ自体が破棄されており、SearchState の
    // generation は 1 から始まるため、cache.gen == search_generation_ のみで
    // キャッシュ有効性を完全判定できる。
    if (cache.gen == search_generation_) {
        return;
    }

    MENDO_COUNT_INC(g_cmd_gen_stats.search_hl_rebuild);
    cache.rects.clear();
    cache.rect_ends.clear();
    cache.rect_ends.reserve(node_match_count);

    for (size_t mi = first_global; mi < first_global + node_match_count; ++mi) {
        const auto& m = matches[mi];
        IDWriteTextLayout* l = nullptr;
        if (m.table_row >= 0 && entry.has_table_layout()) {
            l = entry.table_layout->GetCellLayout(static_cast<size_t>(m.table_row), static_cast<size_t>(m.table_col));
        }
        else if (m.table_row < 0) {
            l = entry.text_layout.Get();
        }
        // m.table_row >= 0 && !has_table_layout() のケースは layout 失効中の
        // 暫定状態で、l は nullptr のまま空 rect として記録する（次回再構築時に修復）。
        if (l && m.length_w > 0) {
            CollectHitTestRects(l, m.start_w, m.length_w, cache.rects);
        }
        else if (!l && m.table_row >= 0) {
            MENDO_COUNT_INC(g_cmd_gen_stats.search_hl_provisional);
        }
        cache.rect_ends.push_back(static_cast<uint32_t>(cache.rects.size()));
    }
    cache.gen = search_generation_;
}

void CommandGenerator::EmitSearchHlCommands(
    DrawCommandList& cmds,
    const SearchHlCache& cache,
    std::span<const SearchMatch> matches,
    size_t first_global,
    float origin_x,
    float origin_y,
    int table_row,
    int table_col)
{
    // 呼び出し側（セル/ノード本体）に属する match のみ描画する。
    const size_t node_match_count = cache.rect_ends.size();

    for (size_t node_mi = 0; node_mi < node_match_count; ++node_mi) {
        const size_t mi = first_global + node_mi;
        const auto& m = matches[mi];
        const bool is_here = (table_row >= 0) ? (m.table_row == table_row && m.table_col == table_col) : (m.table_row < 0);
        if (!is_here) {
            continue;
        }

        const uint32_t rb = (node_mi == 0) ? 0 : cache.rect_ends[node_mi - 1];
        const uint32_t re = cache.rect_ends[node_mi];

        const bool is_current = (static_cast<int>(mi) == current_match_index_);
        const D2D1_COLOR_F color = is_current ? theme_->search_highlight_current_color : theme_->search_highlight_color;
        const BrushId hl_brush = is_current ? BrushId::SearchHighlightCurrent : BrushId::SearchHighlight;
        for (uint32_t k = rb; k < re; ++k) {
            cmds.emplace_back(FillRectCmd{ OffsetRectF(cache.rects[k], origin_x, origin_y), color, hl_brush });
        }
    }
}
