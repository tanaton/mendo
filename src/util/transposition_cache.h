#pragma once
#include <algorithm>
#include <cstddef>
#include <iterator>
#include <utility>
#include <vector>

// 固定長キャッシュ。LRU ではなく Self-organizing list (transposition rule) で
// アクセス順を整列させる。Find / Insert (既存キー) はヒットしたエントリを
// 1 つ前と swap して promote。新規 Insert は先頭に入れ、溢れた末尾を捨てる。
// 挿入は PNG のデコードやキャプチャの後にしか起きないので、詰め直しの手間は問題にならない。
// 非スレッド安全 (Find / Insert は内部順序を変更する)。
template <typename Key, typename Value, size_t MaxEntries>
class TranspositionCache {
    static_assert(MaxEntries > 0, "TranspositionCache: MaxEntries must be greater than 0");

public:
    Value* Find(const Key& key)
    {
        const auto it = std::ranges::find(entries_, key, &Entry::key);
        if (it == entries_.end()) {
            return nullptr;
        }
        if (it == entries_.begin()) {
            return &it->value;
        }
        const auto prev = std::prev(it);
        std::swap(*it, *prev);
        return &prev->value;
    }

    void Insert(const Key& key, Value value)
    {
        if (auto* slot = Find(key); slot) {
            *slot = std::move(value);
            return;
        }
        if (entries_.size() == MaxEntries) {
            entries_.pop_back();
        }
        entries_.insert(entries_.begin(), Entry{ key, std::move(value) });
    }

    // 件数上限だけではビットマップ等の巨大値でメモリが青天井になるため、cost 合計が
    // budget 以下になるまで末尾 (最も使われていない側) から捨てる。先頭 1 件は常に残す。
    template <class CostFn>
    void TrimToBudget(size_t budget, CostFn cost)
    {
        size_t total = 0;
        for (const auto& e : entries_) {
            total += cost(e.value);
        }
        while (entries_.size() > 1 && total > budget) {
            total -= cost(entries_.back().value);
            entries_.pop_back();
        }
    }

    void Clear()
    {
        entries_.clear();
    }

    size_t Size() const noexcept
    {
        return entries_.size();
    }

private:
    struct Entry {
        Key key;
        Value value;
    };

    std::vector<Entry> entries_;
};
