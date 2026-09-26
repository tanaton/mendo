#pragma once
#include "task_scheduler.h"
#include <algorithm>
#include <atomic>
#include <cstddef>
#include <memory>
#include <windows.h>

// [0, count) を grain 単位の chunk に分け、scheduler の worker と呼び出し元で分担して fn(begin, end) を実行する。
// 呼び出し元自身も chunk を取りに行くため、同じ scheduler の worker 上から呼んでもデッドロックしない
// (着手前の helper は chunk 枯渇を見て fn に触れず即 return する)。fn は chunk 間で排他不要であること。
template <class Fn>
void ParallelFor(TaskScheduler* scheduler, size_t count, size_t grain, Fn&& fn)
{
    if (count == 0) {
        return;
    }
    grain = std::max<size_t>(grain, 1);
    const size_t chunk_count = (count + grain - 1) / grain;
    const size_t helper_count = scheduler ? std::min(scheduler->WorkerCount(), chunk_count - 1) : 0;
    if (helper_count == 0) {
        fn(size_t{ 0 }, count);
        return;
    }

    struct State {
        std::atomic<size_t> next{ 0 };
        std::atomic<size_t> done{ 0 };
        size_t chunk_count = 0;
        size_t count = 0;
        size_t grain = 0;
        std::remove_reference_t<Fn>* fn = nullptr;
    };
    auto state = std::make_shared<State>();
    state->chunk_count = chunk_count;
    state->count = count;
    state->grain = grain;
    state->fn = &fn;

    const auto drain = [](State& s) {
        for (size_t k = s.next.fetch_add(1); k < s.chunk_count; k = s.next.fetch_add(1)) {
            const size_t begin = k * s.grain;
            const size_t end = std::min(begin + s.grain, s.count);
            try {
                (*s.fn)(begin, end);
            } catch (...) {
                OutputDebugStringW(L"[mendo] ParallelFor chunk threw exception\n");
            }
            if (s.done.fetch_add(1) + 1 == s.chunk_count) {
                s.done.notify_all();
            }
        }
    };

    for (size_t i = 0; i < helper_count; ++i) {
        // Post 失敗時は呼び出し元が残りを消化するので無視してよい。
        scheduler->Post([state, drain] { drain(*state); });
    }
    drain(*state);
    for (size_t d = state->done.load(); d < chunk_count; d = state->done.load()) {
        state->done.wait(d);
    }
}
