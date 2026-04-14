#pragma once

#include <cstdint>
#include <vector>
#include <ranges>
#include <algorithm>
#include <tuple>
#include <memory_resource>

// ソート済み pmr::vector ベースの連想コンテナ。
// std::map / std::unordered_map と異なりメモリが連続するため、
// 小〜中規模（数百エントリ以下）で CPU キャッシュ効率が高い。
// 挿入・削除は O(n) だが、検索は小規模で線形、大規模で O(log n)。
template <typename Key, typename T>
class FlatMap {
public:
    using key_type = Key;
    using mapped_type = T;
    using value_type = std::pair<key_type, mapped_type>;
    using key_container_type = std::pmr::vector<key_type>;
    using mapped_container_type = std::pmr::vector<mapped_type>;

    struct containers {
        key_container_type keys;
        mapped_container_type values;
    };

    static constexpr std::size_t linear_search_threshold{ 16 };

private:
    containers c;

    // キーを探す。end以外が返った際はキーと戻り値が一致することを比較して確定すること。
    constexpr auto find_key(this auto& self, const key_type& key) noexcept {
        auto iter = self.c.keys.begin();
        const auto end = self.c.keys.end();
        if (self.c.keys.size() < linear_search_threshold) {
            while (iter != end && *iter < key) {
                (void)++iter;
            }
        }
        else {
            iter = std::ranges::lower_bound(iter, end, key);
        }
        return iter;
    }

public:
    constexpr auto zip(this auto& self) noexcept {
        return std::ranges::zip_view{ self.c.keys, self.c.values };
    }
    constexpr auto begin(this auto& self) noexcept {
        return self.zip().begin();
    }
    constexpr auto end(this auto& self) noexcept {
        return self.zip().end();
    }
    constexpr auto cbegin() const noexcept {
        return zip().cbegin();
    }
    constexpr auto cend() const noexcept {
        return zip().cend();
    }

    constexpr auto find(this auto& self, const key_type& key) noexcept {
        const auto key_it = self.find_key(key);
        if (key_it != self.c.keys.end() && *key_it == key) {
            return self.begin() + (key_it - self.c.keys.begin());
        }
        return self.end();
    }

    constexpr bool contains(const key_type& key) const noexcept {
        const auto key_it{ find_key(key) };
        return key_it != c.keys.cend() && *key_it == key;
    }
    constexpr bool empty() const noexcept {
        return c.keys.empty();
    }
    constexpr size_t size() const noexcept {
        return c.keys.size();
    }
    constexpr void clear() noexcept {
        c.keys.clear();
        c.values.clear();
    }
    constexpr void reserve(size_t n) {
        c.keys.reserve(n);
        c.values.reserve(n);
    }
    constexpr void shrink_to_fit() {
        c.keys.shrink_to_fit();
        c.values.shrink_to_fit();
    }

    template <class... Args>
    constexpr auto try_emplace(const key_type& key, Args&&... args) {
        const auto key_it{ find_key(key) };
        const auto dist{ key_it - c.keys.begin() };
        if (key_it != c.keys.end() && *key_it == key) {
            return std::pair{ begin() + dist, false };
        }
        c.keys.insert(key_it, key);
        try {
            c.values.emplace(c.values.begin() + dist, std::forward<Args>(args)...);
        } catch (...) {
            c.keys.erase(c.keys.begin() + dist);
            throw;
        }
        return std::pair{ begin() + dist, true };
    }

    template <class M>
    constexpr auto insert_or_assign(const key_type& key, M&& m) {
        const auto key_it{ find_key(key) };
        const auto dist{ key_it - c.keys.begin() };
        if (key_it != c.keys.end() && *key_it == key) {
            c.values[dist] = std::forward<M>(m);
            return std::pair{ begin() + dist, false };
        }
        c.keys.insert(key_it, key);
        try {
            c.values.emplace(c.values.begin() + dist, std::forward<M>(m));
        } catch (...) {
            c.keys.erase(c.keys.begin() + dist);
            throw;
        }
        return std::pair{ begin() + dist, true };
    }

    template <class Iterator>
    constexpr auto erase(const Iterator it) {
        if (auto e{ end() }; it == e) {
            return e;
        }
        const auto dist{ it - begin() };
        c.keys.erase(c.keys.begin() + dist);
        c.values.erase(c.values.begin() + dist);
        return begin() + dist;
    }

    constexpr auto erase(const key_type& key) {
        const auto key_it{ find_key(key) };
        if (key_it != c.keys.end() && *key_it == key) {
            const auto dist{ key_it - c.keys.begin() };
            c.keys.erase(key_it);
            c.values.erase(c.values.begin() + dist);
            return begin() + dist;
        }
        return end();
    }

    constexpr void swap(FlatMap& other) noexcept {
        std::ranges::swap(c.keys, other.c.keys);
        std::ranges::swap(c.values, other.c.values);
    }
};
