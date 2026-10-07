#include "parallel_measure.h"
#include "layout_computer.h"
#include "parallel_for.h"
#include "profiler.h"
#include "task_scheduler.h"
#include <algorithm>
#include <atomic>
#include <ranges>
#include <cstdint>
#include <windows.h>

namespace mendo::layout {

namespace {

// 16-512 の幅は post 回数と worker 利用率の折衷。下限は巨大 dirty で post を抑え、
// 上限は数百件の dirty でも複数 worker に行き渡らせるための上限。
constexpr size_t kMinChunkSize = 16;
constexpr size_t kMaxChunkSize = 512;

// 戻り値は例外で計測できず dirty のまま再試行を待つノード数。
int MeasureChunk(
    std::pmr::vector<Node>& nodes,
    LayoutCache& cache,
    float content_width,
    const Theme& theme,
    const IMeasureBackend& backend,
    std::span<const size_t> chunk_indices,
    std::span<std::pmr::vector<SyntaxToken>> chunk_slot_tokens,
    MeasureViewportRange viewport)
{
    // 例外はノード単位で捕まえる。chunk 単位だと失敗ノードを特定できず、同じ chunk の残りも計測されない。
    // 失敗回数は worker が担当する自エントリにだけ書くので同期は要らない。
    int failed = 0;
    for (auto&& [i, tokens] : std::views::zip(chunk_indices, chunk_slot_tokens)) {
        auto& entry = cache[i];
        const float indent = NodeIndent(nodes[i], theme);
        try {
            MeasureEntry(backend, nodes[i], entry, content_width - indent, &tokens, viewport, cache.Top(i));
            entry.measure_failures = 0;
        } catch (...) {
            if (++entry.measure_failures >= LayoutCache::kMaxMeasureAttempts) {
                entry.layout_dirty = false;
            }
            else {
                ++failed;
            }
        }
    }
    return failed;
}

} // namespace

int MeasureIndicesParallel(
    std::pmr::vector<Node>& nodes,
    LayoutCache& cache,
    float content_width,
    const Theme& theme,
    const IMeasureBackend& backend,
    std::span<const size_t> indices,
    MeasureViewportRange measure_vp,
    TaskScheduler* scheduler,
    size_t min_parallel)
{
    // slot k は indices[k] に対応。worker は自スロットへ書き、UI スレッドで Node に集約する。
    // 事前サイズ確定なので chunk 完了順に依存せず、sort も merge も不要。
    std::pmr::vector<std::pmr::vector<SyntaxToken>> slot_tokens(
        indices.size(), std::pmr::get_default_resource());

    const size_t worker_count = scheduler ? std::max<size_t>(scheduler->WorkerCount(), 1) : 1;
    const size_t chunk_size = indices.size() < min_parallel
                                  ? indices.size()
                                  : std::clamp(indices.size() / (worker_count * 4), kMinChunkSize, kMaxChunkSize);
    if (!indices.empty()) {
        const auto [lo, hi] = std::ranges::minmax(indices);
        cache.NoteMaterialized(lo, hi);
    }
    std::atomic<int> failed_node_count{ 0 };
    ParallelFor(scheduler, indices.size(), chunk_size, [&](size_t begin, size_t end) {
        MENDO_PROFILE("MeasureNode.chunk");
        const int failed_in_chunk = MeasureChunk(nodes, cache, content_width, theme, backend, indices.subspan(begin, end - begin),
                                                 std::span(slot_tokens).subspan(begin, end - begin), measure_vp);
        if (failed_in_chunk > 0) {
            failed_node_count.fetch_add(failed_in_chunk, std::memory_order_relaxed);
        }
    });
    const int failed = failed_node_count.load(std::memory_order_relaxed);
    if (failed > 0) {
        OutputDebugStringW(L"[mendo] MeasureIndicesParallel: node measure threw exception\n");
    }
    MENDO_PLOT("layout.parallel.error_count", static_cast<int64_t>(failed));

    {
        MENDO_PROFILE("MeasureIndicesParallel.Aggregate");
        // 非 CodeBlock や既トークン化済みノードでは MeasureNode が tokens_out に書かない。
        // 空 vector で既存トークンを上書きすると zoom/theme 変更後にハイライトが失われる。
        for (auto&& [i, tokens] : std::views::zip(indices, slot_tokens)) {
            if (!tokens.empty()) {
                nodes[i].syntax_tokens_mut() = std::move(tokens);
            }
        }
    }
    return failed;
}

DirtyBatchResult RunParallel(
    std::pmr::vector<Node>& nodes,
    LayoutCache& cache,
    float content_width,
    const Theme& theme,
    const IMeasureBackend& backend,
    ViewportClip clip,
    ParallelBudget budget,
    TaskScheduler& scheduler)
{
    MENDO_PROFILE("layout::RunParallel");
    DirtyBatchResult result;
    const auto node_count = nodes.size();
    const MeasureViewportRange range = clip.Range();
    // ParallelBudget には time_us が無い (シグネチャで明示)。
    // worker 側に polling checkpoint が無いため、time-based 制御は RunSerial 専用。
    const bool has_batch_limit = (budget.max_nodes > 0);

    std::pmr::vector<size_t> indices(std::pmr::get_default_resource());
    {
        MENDO_PROFILE("RunParallel.Plan");
        // 上限が無ければ最悪ケース (全ノード dirty) で予約し、push_back 中の再確保を避ける。
        indices.reserve(has_batch_limit ? std::min(node_count, static_cast<size_t>(budget.max_nodes)) : node_count);
        // text_top は単調なので、帯の開始は二分探索で求め、下端超過で break する。
        // 全走査 + reserve(node_count) は 100MB 級文書で 16ms タイマーごとに
        // 数 MB の確保と全エントリ読みを繰り返してしまう。
        const auto plan_begin = static_cast<size_t>(FindFirstVisibleNodeIndex(cache, node_count, range.top));
        for (size_t i = plan_begin; i < node_count; i++) {
            const float entry_top = cache.Top(i);
            if (entry_top > range.bottom) {
                break;
            }
            if (!ViewportClip::ShouldMeasure(cache[i], entry_top, range)) {
                continue;
            }
            indices.push_back(i);
            if (has_batch_limit && static_cast<int>(indices.size()) >= budget.max_nodes) {
                result.reason = StopReason::BatchLimit;
                break;
            }
        }
    }

    if (indices.empty()) {
        result.reason = StopReason::NoneDirty;
        return result;
    }

    if (result.reason == StopReason::NoneDirty) {
        result.reason = StopReason::Done;
    }
    result.first_processed = indices.front();
    result.last_processed = indices.back();
    result.processed = static_cast<int>(indices.size());

    const int failed = MeasureIndicesParallel(nodes, cache, content_width, theme, backend, indices, range, &scheduler);
    if (failed > 0) {
        // 失敗分は processed から外し、any_nearby_skipped() 経由で次フレーム再試行に乗せる。
        result.processed -= failed;
        if (result.reason == StopReason::Done) {
            result.reason = StopReason::Error;
        }
    }

    MENDO_PLOT("layout.parallel.dirty_count", static_cast<int64_t>(result.processed));
    return result;
}

} // namespace mendo::layout
