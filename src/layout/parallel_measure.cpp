#include "parallel_measure.h"
#include "layout_computer.h"
#include "profiler.h"
#include "task_scheduler.h"
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <latch>
#include <vector>
#include <windows.h>

namespace mendo::layout {

namespace {

// 16-512 の幅は post 回数と worker 利用率の折衷。下限は巨大 dirty で post を抑え、
// 上限は数百件の dirty でも複数 worker に行き渡らせるための上限。
constexpr size_t kMinChunkSize = 16;
constexpr size_t kMaxChunkSize = 512;
// dispatch + latch のオーバーヘッドが per-node 並列利得を上回る境界。
constexpr size_t kMinDirtyForParallel = 32;

void MeasureChunk(std::pmr::vector<Node>& nodes,
                  LayoutCache& cache,
                  float content_width,
                  const Theme& theme,
                  const IMeasureBackend& backend,
                  std::span<const size_t> chunk_indices,
                  std::span<std::pmr::vector<SyntaxToken>> chunk_slot_tokens)
{
    for (size_t k = 0; k < chunk_indices.size(); ++k) {
        const size_t i = chunk_indices[k];
        const float indent = NodeIndent(nodes[i], theme);
        backend.MeasureNode(nodes[i], cache[i], content_width - indent, &chunk_slot_tokens[k]);
    }
}

} // namespace

DirtyBatchResult RunParallel(std::pmr::vector<Node>& nodes,
                             LayoutCache& cache,
                             float content_width,
                             const Theme& theme,
                             const IMeasureBackend& backend,
                             ViewportClip clip,
                             DirtyBudget budget,
                             TaskScheduler& scheduler)
{
    MENDO_PROFILE("DirtyScheduler::RunParallel");
    DirtyBatchResult result;
    const auto node_count = nodes.size();

    const bool has_viewport_limit = clip.active();
    const float limit_top = has_viewport_limit ? clip.limit_top() : 0.0f;
    const float limit_bottom = has_viewport_limit ? clip.limit_bottom() : 0.0f;
    // time_us 無視: worker 側に polling checkpoint が無いので RunSerial と非対称。
    const bool has_batch_limit = (budget.max_nodes > 0);

    std::pmr::vector<size_t> indices(std::pmr::get_default_resource());
    // 最悪ケースは全ノード dirty。size_t 8B × 数千 ≒ 数十 KB で global arena には軽い。
    // 過小予約による push_back 中の再確保を避けるほうが利得が大きい。
    indices.reserve(node_count);
    {
        MENDO_PROFILE("RunParallel.Plan");
        for (size_t i = 0; i < node_count; i++) {
            const auto& entry = cache[i];
            if (!ViewportClip::ShouldMeasure(entry, has_viewport_limit, limit_top, limit_bottom)) {
                continue;
            }
            indices.push_back(i);
            if (has_batch_limit && static_cast<int>(indices.size()) >= budget.max_nodes) {
                result.reason = StopReason::BatchLimit;
                result.any_nearby_skipped = true;
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

    // slot k は indices[k] に対応。worker は自スロットへ書き、UI スレッドで Node に集約する。
    // 事前サイズ確定なので chunk 完了順に依存せず、sort も merge も不要。
    std::pmr::vector<std::pmr::vector<SyntaxToken>> slot_tokens(
        indices.size(), std::pmr::get_default_resource());

    if (indices.size() < kMinDirtyForParallel) {
        MENDO_PROFILE("RunParallel.Inline");
        MeasureChunk(nodes, cache, content_width, theme, backend, indices,
                     { slot_tokens.data(), slot_tokens.size() });
    }
    else {
        const size_t worker_count = std::max<size_t>(scheduler.WorkerCount(), 1);
        const size_t target_chunks = worker_count * 4;
        const size_t chunk_size = std::clamp(indices.size() / target_chunks, kMinChunkSize, kMaxChunkSize);
        const size_t chunk_count = (indices.size() + chunk_size - 1) / chunk_size;

        std::latch latch(static_cast<ptrdiff_t>(chunk_count));
        std::atomic<int> failed_node_count{ 0 };

        // 例外が latch.count_down() の前で抜けると wait() が永久ブロックするので必ず try/catch で覆う。
        auto run_chunk = [&](std::span<const size_t> ci, std::span<std::pmr::vector<SyntaxToken>> co) {
            try {
                MeasureChunk(nodes, cache, content_width, theme, backend, ci, co);
            } catch (...) {
                failed_node_count.fetch_add(static_cast<int>(ci.size()), std::memory_order_relaxed);
                OutputDebugStringW(L"[mendo] RunParallel chunk threw exception\n");
            }
            latch.count_down();
        };

        {
            MENDO_PROFILE("RunParallel.Dispatch");
            for (size_t chunk_idx = 0; chunk_idx < chunk_count; chunk_idx++) {
                const size_t begin = chunk_idx * chunk_size;
                const size_t end = std::min(begin + chunk_size, indices.size());
                const std::span<const size_t> chunk_indices{ indices.data() + begin, end - begin };
                const std::span<std::pmr::vector<SyntaxToken>> chunk_slots{ slot_tokens.data() + begin, end - begin };
                const bool posted = scheduler.Post([&, chunk_indices, chunk_slots]() {
                    MENDO_PROFILE("MeasureNode.worker");
                    run_chunk(chunk_indices, chunk_slots);
                });
                if (!posted) {
                    MENDO_PROFILE("MeasureNode.fallback");
                    run_chunk(chunk_indices, chunk_slots);
                }
            }
        }

        {
            MENDO_PROFILE("RunParallel.Wait");
            latch.wait();
        }

        const int failed = failed_node_count.load(std::memory_order_relaxed);
        if (failed > 0) {
            // 失敗分は processed から外し、any_nearby_skipped 経由で次フレーム再試行に乗せる。
            result.processed -= failed;
            if (result.reason == StopReason::Done) {
                result.reason = StopReason::Error;
            }
            result.any_nearby_skipped = true;
        }

        MENDO_PLOT("layout.parallel.chunk_count", static_cast<int64_t>(chunk_count));
        MENDO_PLOT("layout.parallel.error_count", static_cast<int64_t>(failed));
    }

    {
        MENDO_PROFILE("RunParallel.Aggregate");
        for (size_t k = 0; k < indices.size(); ++k) {
            if (!slot_tokens[k].empty()) {
                nodes[indices[k]].syntax_tokens_mut() = std::move(slot_tokens[k]);
            }
        }
    }

    MENDO_PLOT("layout.parallel.dirty_count", static_cast<int64_t>(result.processed));
    return result;
}

} // namespace mendo::layout
