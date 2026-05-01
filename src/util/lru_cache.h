#pragma once
#include <vector>
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <ranges>

// 固定サイズの LRU (Least Recently Used) キャッシュ。
// キーと値を連続メモリ上に保持し、CPU キャッシュの局所性を最大化する。
// Find() でアクセスするとアクセス世代が更新される。
// 容量超過時は最も古いエントリが自動的に破棄される。
// 検索は O(n) だが、エントリ数が数百以下であれば
// std::list ベースの O(1) LRU よりキャッシュラインの恩恵で高速。
// スレッド安全ではない。const Find() も内部の世代を変更するため、
// 複数スレッドからの同時アクセスには外部で排他制御が必要。
template <typename Key, typename Value>
class LruCache {
public:
    constexpr explicit LruCache(size_t max_entries) : max_entries_(max_entries)
    {
        keys_.reserve(max_entries);
        values_.reserve(max_entries);
        generations_.reserve(max_entries);
    }

    constexpr auto* Find(this auto& self, const Key& key)
    {
        for (size_t i = 0; i < self.size_; i++) {
            if (self.keys_[i] == key) {
                self.generations_[i] = ++self.generation_counter_;
                return &self.values_[i];
            }
        }
        return decltype(&self.values_[0]){ nullptr };
    }

    constexpr bool Contains(const Key& key) const
    {
        return std::ranges::contains(keys_ | std::views::take(size_), key);
    }

    constexpr void Insert(const Key& key, Value value)
    {
        if (max_entries_ == 0) {
            return;
        }

        for (size_t i = 0; i < size_; i++) {
            if (keys_[i] == key) {
                values_[i] = std::move(value);
                generations_[i] = ++generation_counter_;
                return;
            }
        }

        if (size_ >= max_entries_) {
            const size_t oldest = FindOldestIndex();
            keys_[oldest] = key;
            values_[oldest] = std::move(value);
            generations_[oldest] = ++generation_counter_;
            return;
        }

        keys_.push_back(key);
        values_.push_back(std::move(value));
        generations_.push_back(++generation_counter_);
        ++size_;
    }

    constexpr void Clear()
    {
        keys_.clear();
        values_.clear();
        generations_.clear();
        size_ = 0;
        generation_counter_ = 0;
    }

    constexpr size_t Size() const noexcept
    {
        return size_;
    }
    constexpr bool Empty() const noexcept
    {
        return size_ == 0;
    }
    constexpr size_t MaxSize() const noexcept
    {
        return max_entries_;
    }

private:
    constexpr size_t FindOldestIndex() const noexcept
    {
        size_t oldest = 0;
        uint64_t min_gen = generations_[0];
        for (size_t i = 1; i < size_; i++) {
            if (generations_[i] < min_gen) {
                min_gen = generations_[i];
                oldest = i;
            }
        }
        return oldest;
    }

    std::vector<Key> keys_;
    std::vector<Value> values_;
    mutable std::vector<uint64_t> generations_;
    mutable uint64_t generation_counter_ = 0;
    size_t size_ = 0;
    size_t max_entries_;
};
