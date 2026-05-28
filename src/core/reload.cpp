#include "reload.h"
#include "layout_cache.h"
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <ranges>

size_t FindFirstDifference(std::string_view old_text, std::string_view new_text) noexcept
{
    const size_t min_len = std::min(old_text.size(), new_text.size());
    constexpr size_t kChunk = 64;
    size_t i = 0;
    for (; i + kChunk <= min_len; i += kChunk) {
        if (std::memcmp(old_text.data() + i, new_text.data() + i, kChunk) != 0) {
            break;
        }
    }
    for (; i < min_len; ++i) {
        if (old_text[i] != new_text[i]) {
            return i;
        }
    }
    if (old_text.size() != new_text.size()) {
        return min_len;
    }
    return std::string_view::npos;
}

ReloadDecision AnalyzeReloadDiff(std::string_view old_text, std::string_view new_text) noexcept
{
    const size_t diff_pos = FindFirstDifference(old_text, new_text);
    if (diff_pos == std::string_view::npos) {
        return { ReloadOp::NoChange, std::string_view::npos };
    }
    if (IsPrefixOnlyDiff(diff_pos, old_text.size(), new_text.size())) {
        if (new_text.size() < old_text.size()) {
            return { ReloadOp::DeferPrefixShrink, diff_pos };
        }
        return { ReloadOp::PrefixGrowth, diff_pos };
    }
    return { ReloadOp::FullReload, diff_pos };
}

int FindNodeBySourceOffset(const std::pmr::vector<Node>& nodes, const char* raw_base, size_t diff_offset) noexcept
{
    // source_offset はパース順で基本的に単調増加するため二分探索を使用。
    // 未設定ノードに当たった場合は左に有効ノードを探してから判定する。
    int lo = 0, hi = static_cast<int>(nodes.size()) - 1;
    int result = -1;
    while (lo <= hi) {
        const int mid = lo + (hi - lo) / 2;
        const auto offset = nodes[mid].SourceOffsetFrom(raw_base);
        if (offset == kUnsetSourceOffset) {
            int probe = mid - 1;
            while (probe >= lo && !nodes[probe].HasSourceOffset()) {
                probe--;
            }
            if (probe < lo) {
                lo = mid + 1;
            }
            else {
                const auto probe_offset = nodes[probe].SourceOffsetFrom(raw_base);
                if (probe_offset <= diff_offset) {
                    result = probe;
                    lo = mid + 1;
                }
                else {
                    hi = probe - 1;
                }
            }
            continue;
        }
        if (offset <= diff_offset) {
            result = mid;
            lo = mid + 1;
        }
        else {
            hi = mid - 1;
        }
    }
    return result;
}

float CalcScrollYForDiff(
    const std::pmr::vector<Node>& nodes,
    const LayoutCache& cache,
    std::string_view content,
    size_t diff_pos,
    float viewport_height,
    float fallback_scroll) noexcept
{
    const char* const raw_base = content.data();
    const auto changed_node = FindNodeBySourceOffset(nodes, raw_base, diff_pos);
    if (changed_node < 0 || changed_node >= static_cast<int>(cache.size())) {
        return fallback_scroll;
    }

    // ノード上端 Y は cache[i].text_top フィールドを直読する。Fenwick PrefixSum 経由
    // (TextTopOf) は float 加算順が違うためノード数が増えると誤差が累積し、
    // ファイル下部更新時に着地点が大きくズレる。
    float node_y = cache[changed_node].text_top;
    const float node_h = cache[changed_node].height;

    const auto node_start = nodes[changed_node].SourceOffsetFrom(raw_base);
    if (node_start != kUnsetSourceOffset) {
        auto next_start = content.size();
        const auto node_count = static_cast<int>(nodes.size());
        // 途中に offset 無しノード (HR や空のルーズリスト等) が連続しても次の有効 offset を取り逃さない
        // よう探索する。末尾まで線形走査すると大規模ファイルのリロードが重くなるため上限を設ける
        // (通常は最初の有効 offset で break するため上限には届かない)。
        constexpr int kMaxOffsetProbe = 4096;
        const int probe_limit = std::min(node_count, changed_node + 1 + kMaxOffsetProbe);
        for (int i = changed_node + 1; i < probe_limit; ++i) {
            const auto off = nodes[i].SourceOffsetFrom(raw_base);
            if (off != kUnsetSourceOffset && off > node_start) {
                next_start = off;
                break;
            }
        }
        if (next_start > node_start) {
            const float fraction = static_cast<float>(diff_pos - node_start) / static_cast<float>(next_start - node_start);
            node_y += node_h * std::min(fraction, 1.0f);
        }
    }

    const float margin = viewport_height * 0.2f;
    return std::max(0.0f, node_y - margin);
}
