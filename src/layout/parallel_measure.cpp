#include "parallel_measure.h"
#include "layout_computer.h"
#include "profiler.h"
#include "task_scheduler.h"
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <latch>
#include <ranges>
#include <vector>
#include <windows.h>

namespace mendo::layout {

namespace {

struct TokenizeResult {
    size_t node_index = 0;
    std::pmr::vector<SyntaxToken> tokens;

    TokenizeResult() : tokens(std::pmr::get_default_resource()) {}
};

// chunk size の上下限。小規模 (200 件程度) でも複数 worker に分散できるよう
// 上限を緩く取り、巨大 (22000 件) でも post 回数が膨らみすぎないよう下限で締める。
constexpr size_t kMinChunkSize = 16;
constexpr size_t kMaxChunkSize = 512;
// indices サイズがこれ未満なら chunk 化せず inline 直列で処理する。
// dispatch + latch + sort のオーバーヘッドが per-node 並列利得を上回る境界。
constexpr size_t kMinDirtyForParallel = 32;

// chunk 内の MeasureNode 呼び出し本体。worker / fallback / inline で共通利用する。
void MeasureChunk(std::pmr::vector<Node>& nodes,
                  LayoutCache& cache,
                  float content_width,
                  const Theme& theme,
                  const IMeasureBackend& backend,
                  std::span<const size_t> chunk_indices,
                  std::vector<TokenizeResult>& out_tokens)
{
    std::pmr::vector<SyntaxToken> tokens_buf(std::pmr::get_default_resource());
    for (size_t i : chunk_indices) {
        auto& entry = cache[i];
        const float indent = NodeIndent(nodes[i], theme);
        tokens_buf.clear();
        backend.MeasureNode(nodes[i], entry, content_width - indent, &tokens_buf);
        if (!tokens_buf.empty()) {
            TokenizeResult tr;
            tr.node_index = i;
            tr.tokens = std::move(tokens_buf);
            out_tokens.push_back(std::move(tr));
        }
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

    const bool has_viewport_limit = (clip.top >= 0.0f && clip.height > 0.0f);
    const float limit_top = has_viewport_limit ? clip.top - clip.height * clip.buffer_screens : 0.0f;
    const float limit_bottom = has_viewport_limit ? clip.top + clip.height + clip.height * clip.buffer_screens : 0.0f;
    const bool has_batch_limit = (budget.max_nodes > 0);

    std::pmr::vector<size_t> indices(std::pmr::get_default_resource());
    indices.reserve(node_count / 8 + 16);
    {
        MENDO_PROFILE("RunParallel.Plan");
        for (size_t i = 0; i < node_count; i++) {
            const auto& entry = cache[i];
            if (!entry.layout_dirty) {
                continue;
            }
            if (has_viewport_limit && IsOffscreen(entry.y_position, entry.height, limit_top, limit_bottom)) {
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

    // 集約バッファ。inline / parallel どちらの経路でも最終的にここに集まる。
    std::pmr::vector<TokenizeResult> flat_results(std::pmr::get_default_resource());

    if (indices.size() < kMinDirtyForParallel) {
        // 小規模 dirty: dispatch コストを避けて UI スレッドで直列処理。
        MENDO_PROFILE("RunParallel.Inline");
        flat_results.reserve(indices.size() / 4 + 4);
        std::vector<TokenizeResult> tmp;
        MeasureChunk(nodes, cache, content_width, theme, backend, indices, tmp);
        for (auto& tr : tmp) {
            flat_results.push_back(std::move(tr));
        }
    }
    else {
        // 1 worker あたり ~4 chunk を目標に動的サイズ。worker=8 / N=200 → chunk_size=25 → 8 chunk、
        // worker=8 / N=22000 → chunk_size=512 (max) → 43 chunk。
        const size_t worker_count = std::max<size_t>(scheduler.WorkerCount(), 1);
        const size_t target_chunks = worker_count * 4;
        const size_t chunk_size = std::clamp(indices.size() / target_chunks, kMinChunkSize, kMaxChunkSize);
        const size_t chunk_count = (indices.size() + chunk_size - 1) / chunk_size;
        std::vector<std::vector<TokenizeResult>> chunk_buffers(chunk_count);

        std::latch latch(static_cast<ptrdiff_t>(chunk_count));
        std::atomic<int> error_count{ 0 };

        {
            MENDO_PROFILE("RunParallel.Dispatch");
            for (size_t chunk_idx = 0; chunk_idx < chunk_count; chunk_idx++) {
                const size_t begin = chunk_idx * chunk_size;
                const size_t end = std::min(begin + chunk_size, indices.size());
                const std::span<const size_t> chunk_indices{ indices.data() + begin, end - begin };
                auto& buf = chunk_buffers[chunk_idx];
                const bool posted = scheduler.Post([&, chunk_indices]() {
                    MENDO_PROFILE("MeasureNode.worker");
                    try {
                        MeasureChunk(nodes, cache, content_width, theme, backend, chunk_indices, buf);
                    }
                    catch (...) {
                        error_count.fetch_add(1, std::memory_order_relaxed);
                        OutputDebugStringW(L"[mendo] RunParallel chunk threw exception\n");
                    }
                    latch.count_down();
                });
                if (!posted) {
                    MENDO_PROFILE("MeasureNode.fallback");
                    MeasureChunk(nodes, cache, content_width, theme, backend, chunk_indices, buf);
                    latch.count_down();
                }
            }
        }

        {
            MENDO_PROFILE("RunParallel.Wait");
            latch.wait();
        }

        size_t total = 0;
        for (const auto& buf : chunk_buffers) {
            total += buf.size();
        }
        flat_results.reserve(total);
        for (auto& buf : chunk_buffers) {
            for (auto& tr : buf) {
                flat_results.push_back(std::move(tr));
            }
        }

        MENDO_PLOT("layout.parallel.chunk_count", static_cast<int64_t>(chunk_count));
        MENDO_PLOT("layout.parallel.error_count", static_cast<int64_t>(error_count.load(std::memory_order_relaxed)));
    }

    {
        MENDO_PROFILE("RunParallel.Aggregate");
        // chunk 完了順序は非決定論的なので node_index 昇順に並べてから Node に書き戻す。
        std::ranges::sort(flat_results, {}, &TokenizeResult::node_index);
        for (auto& tr : flat_results) {
            nodes[tr.node_index].syntax_tokens_mut() = std::move(tr.tokens);
        }
    }

    MENDO_PLOT("layout.parallel.dirty_count", static_cast<int64_t>(result.processed));
    return result;
}

} // namespace mendo::layout
