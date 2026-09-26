#include "parallel_measure.h"
#include "layout_computer.h"
#include "parallel_for.h"
#include "profiler.h"
#include "task_scheduler.h"
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <vector>
#include <windows.h>

namespace mendo::layout {

namespace {

// 16-512 の幅は post 回数と worker 利用率の折衷。下限は巨大 dirty で post を抑え、
// 上限は数百件の dirty でも複数 worker に行き渡らせるための上限。
constexpr size_t kMinChunkSize = 16;
constexpr size_t kMaxChunkSize = 512;

void MeasureChunk(
    std::pmr::vector<Node>& nodes,
    LayoutCache& cache,
    float content_width,
    const Theme& theme,
    const IMeasureBackend& backend,
    std::span<const size_t> chunk_indices,
    std::span<std::pmr::vector<SyntaxToken>> chunk_slot_tokens,
    MeasureViewportRange viewport)
{
    for (size_t k = 0; k < chunk_indices.size(); ++k) {
        const size_t i = chunk_indices[k];
        const float indent = NodeIndent(nodes[i], theme);
        MeasureEntry(backend, nodes[i], cache[i], content_width - indent, &chunk_slot_tokens[k], viewport, cache.Top(i));
    }
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
    std::atomic<int> failed_node_count{ 0 };
    ParallelFor(scheduler, indices.size(), chunk_size, [&](size_t begin, size_t end) {
        MENDO_PROFILE("MeasureNode.chunk");
        try {
            MeasureChunk(nodes, cache, content_width, theme, backend, indices.subspan(begin, end - begin),
                         std::span(slot_tokens).subspan(begin, end - begin), measure_vp);
        } catch (...) {
            failed_node_count.fetch_add(static_cast<int>(end - begin), std::memory_order_relaxed);
            OutputDebugStringW(L"[mendo] MeasureIndicesParallel chunk threw exception\n");
        }
    });
    const int failed = failed_node_count.load(std::memory_order_relaxed);
    MENDO_PLOT("layout.parallel.error_count", static_cast<int64_t>(failed));

    {
        MENDO_PROFILE("RunParallel.Aggregate");
        // 非 CodeBlock や既トークン化済みノードでは MeasureNode が tokens_out に書かない。
        // 空 vector で既存トークンを上書きすると zoom/theme 変更後にハイライトが失われる。
        for (size_t k = 0; k < indices.size(); ++k) {
            if (!slot_tokens[k].empty()) {
                nodes[indices[k]].syntax_tokens_mut() = std::move(slot_tokens[k]);
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
    MENDO_PROFILE("DirtyScheduler::RunParallel");
    DirtyBatchResult result;
    const auto node_count = nodes.size();

    const bool has_viewport_limit = clip.active();
    const float limit_top = has_viewport_limit ? clip.limit_top() : 0.0f;
    const float limit_bottom = has_viewport_limit ? clip.limit_bottom() : 0.0f;
    const MeasureViewportRange measure_vp = has_viewport_limit
        ? MeasureViewportRange{ limit_top, limit_bottom }
        : MeasureViewportRange{};
    // ParallelBudget には time_us が無い (シグネチャで明示)。
    // worker 側に polling checkpoint が無いため、time-based 制御は RunSerial 専用。
    const bool has_batch_limit = (budget.max_nodes > 0);

    std::pmr::vector<size_t> indices(std::pmr::get_default_resource());
    {
        MENDO_PROFILE("RunParallel.Plan");
        size_t plan_begin = 0;
        if (has_viewport_limit) {
            // text_top は単調なので、帯の開始は二分探索で求め、下端超過で break する。
            // 全走査 + reserve(node_count) は 100MB 級文書で 16ms タイマーごとに
            // 数 MB の確保と全エントリ読みを繰り返してしまう。
            plan_begin = static_cast<size_t>(FindFirstVisibleNodeIndex(cache, node_count, limit_top));
            indices.reserve(has_batch_limit ? std::min(node_count, static_cast<size_t>(budget.max_nodes)) : node_count);
        }
        else {
            // 最悪ケースは全ノード dirty。size_t 8B × 数千 ≒ 数十 KB で global arena には軽い。
            // 過小予約による push_back 中の再確保を避けるほうが利得が大きい。
            indices.reserve(node_count);
        }
        for (size_t i = plan_begin; i < node_count; i++) {
            const float entry_top = cache.Top(i);
            if (has_viewport_limit && entry_top > limit_bottom) {
                break;
            }
            if (!ViewportClip::ShouldMeasure(cache[i], entry_top, has_viewport_limit, limit_top, limit_bottom)) {
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

    const int failed = MeasureIndicesParallel(nodes, cache, content_width, theme, backend, indices, measure_vp, &scheduler);
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
