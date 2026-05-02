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
// 1 つ前と swap して promote。新規 Insert は先頭挿入で末尾を捨てる。
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
            if (self.keys_[i] == key) {
                if constexpr (!std::is_const_v<std::remove_reference_t<decltype(self)>>) {
                    if (i > 0) {
                        std::ranges::swap(self.keys_[i], self.keys_[i - 1]);
                        std::ranges::swap(self.values_[i], self.values_[i - 1]);
                        return self.values_.data() + (i - 1);
                    }
                }
                return self.values_.data() + i;
            }
        }
        return decltype(self.values_.data()){ nullptr };
    }

    constexpr bool Contains(const Key& key) const
    {
        return std::ranges::contains(keys_.begin(), keys_.begin() + size_, key);
    }

    constexpr void Insert(const Key& key, Value value)
    {
        for (size_t i = 0; i < size_; i++) {
            if (keys_[i] == key) {
                if (i > 0) {
                    keys_[i] = std::move(keys_[i - 1]);
                    values_[i] = std::move(values_[i - 1]);
                    keys_[i - 1] = key;
                    values_[i - 1] = std::move(value);
                }
                else {
                    values_[i] = std::move(value);
                }
                return;
            }
        }

        if (size_ < MaxEntries) {
            ++size_;
        }
        std::ranges::shift_right(keys_.begin(), keys_.begin() + size_, 1);
        std::ranges::shift_right(values_.begin(), values_.begin() + size_, 1);
        keys_[0] = key;
        values_[0] = std::move(value);
    }

    constexpr void Clear()
    {
        keys_ = {};
        values_ = {};
        size_ = 0;
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
    std::array<Key, MaxEntries> keys_{};
    std::array<Value, MaxEntries> values_{};
    size_t size_ = 0;
};
