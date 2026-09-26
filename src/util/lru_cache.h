#pragma once
#include <array>
#include <algorithm>
#include <cstddef>
#include <ranges>
#include <span>
#include <type_traits>
#include <utility>

// 固定長キャッシュ。Self-organizing list (transposition rule) で
// アクセス順を整列させる。Find / Insert (既存キー) はヒットしたエントリを
// 1 つ前と swap して promote。新規 Insert は循環バッファ + 先頭インデックス
// (head_) を 1 つ後ろにずらすことで O(1)。物理位置 = (head_ + 論理位置) % MaxEntries。
// std::array で連続メモリ、ヒープアロケーション 0。
// 非スレッド安全 (非 const Find / Insert は内部順序を変更する)。
template <typename Key, typename Value, size_t MaxEntries>
class LruCache {
    static_assert(MaxEntries > 0, "LruCache: MaxEntries must be greater than 0");

public:
    constexpr LruCache() = default;

    constexpr auto* Find(this auto& self, const Key& key)
    {
        for (size_t i = 0; i < self.size_; i++) {
            const size_t p = self.physical(i);
            if (self.keys_[p] == key) {
                if constexpr (!std::is_const_v<std::remove_reference_t<decltype(self)>>) {
                    if (i > 0) {
                        const size_t prev_p = self.physical(i - 1);
                        std::ranges::swap(self.keys_[p], self.keys_[prev_p]);
                        std::ranges::swap(self.values_[p], self.values_[prev_p]);
                        return self.values_.data() + prev_p;
                    }
                }
                return self.values_.data() + p;
            }
        }
        return decltype(self.values_.data()){ nullptr };
    }

    constexpr bool Contains(const Key& key) const
    {
        for (size_t i = 0; i < size_; i++) {
            if (keys_[physical(i)] == key) {
                return true;
            }
        }
        return false;
    }

    constexpr void Insert(const Key& key, Value value)
    {
        if (auto* slot = Find(key); slot) {
            *slot = std::move(value);
            return;
        }
        head_ = (head_ + MaxEntries - 1) % MaxEntries;
        keys_[head_] = key;
        values_[head_] = std::move(value);
        if (size_ < MaxEntries) {
            ++size_;
        }
    }

    // 件数上限だけではビットマップ等の巨大値でメモリが青天井になるため、cost 合計が
    // budget 以下になるまで論理末尾 (最も使われていない側) から捨てる。先頭 1 件は常に残す。
    template <class CostFn>
    constexpr void TrimToBudget(size_t budget, CostFn cost)
    {
        size_t total = 0;
        for (size_t i = 0; i < size_; i++) {
            total += cost(values_[physical(i)]);
        }
        while (size_ > 1 && total > budget) {
            const size_t p = physical(size_ - 1);
            total -= cost(values_[p]);
            keys_[p] = Key{};
            values_[p] = Value{};
            --size_;
        }
    }

    constexpr void Clear()
    {
        for (size_t i = 0; i < size_; i++) {
            const size_t p = physical(i);
            keys_[p] = Key{};
            values_[p] = Value{};
        }
        size_ = 0;
        head_ = 0;
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
        return MaxEntries;
    }

private:
    constexpr size_t physical(size_t logical) const noexcept
    {
        return (head_ + logical) % MaxEntries;
    }

    std::array<Key, MaxEntries> keys_{};
    std::array<Value, MaxEntries> values_{};
    size_t size_ = 0;
    size_t head_ = 0; // 論理 0 番目に対応する物理インデックス
};
