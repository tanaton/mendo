#pragma once
#include <bit>
#include <cassert>
#include <cstddef>
#include <memory_resource>
#include <span>
#include <vector>

// 一次元の Fenwick tree (BIT) over float。
// Set / Add は O(log N)、PrefixSum / RangeSum も O(log N)。
//
// 用途: LayoutCache のノード block height (= spacing_above + height + spacing_below) を
// インデックス順に集約し、ノード i の Y 位置 = margin_top + PrefixSum(i)、
// 全体高さ = 2 * margin_top + PrefixSum(N) を O(log N) で取得する。
//
// 制約:
// - 末尾追加 (GrowTo) は Fenwick の構造上 0 値追加なら正しい。任意値の追加は
//   GrowTo + Set の二段階で行うこと。
// - 縮小は値を保持しない。サイズが減るときは Reset + Resize で再構築する想定。

namespace mendo {

class FloatFenwick {
public:
    explicit FloatFenwick(std::pmr::memory_resource* mr = std::pmr::get_default_resource()) noexcept
        : tree_(mr)
    {
    }

    FloatFenwick(const FloatFenwick&) = delete;
    FloatFenwick& operator=(const FloatFenwick&) = delete;
    FloatFenwick(FloatFenwick&&) = default;
    FloatFenwick& operator=(FloatFenwick&&) = default;

    // 全要素を 0 にして指定サイズに確保し直す。
    void Resize(std::size_t n)
    {
        tree_.assign(n, 0.0f);
    }

    // サイズ 0 にして capacity を解放する。
    void Reset() noexcept
    {
        tree_.clear();
    }

    // 末尾に 0 値要素を追加してサイズを n まで拡張する (n が現在以下なら no-op)。
    // 単純な resize では Fenwick の累積構造が壊れる (新規 i のノードはその i 位置までの
    // 累積和を保持する必要があるため)。値配列を取り出して Build で再構築する O(N) パス。
    void GrowTo(std::size_t n)
    {
        const std::size_t old = tree_.size();
        if (n <= old) {
            return;
        }
        std::pmr::vector<float> values(tree_.get_allocator().resource());
        values.reserve(n);
        for (std::size_t i = 0; i < old; ++i) {
            values.push_back(GetPoint(i));
        }
        values.resize(n, 0.0f);
        tree_.resize(n);
        Build(std::span<const float>(values.data(), values.size()));
    }

    constexpr std::size_t size() const noexcept
    {
        return tree_.size();
    }

    constexpr bool empty() const noexcept
    {
        return tree_.empty();
    }

    // i 番目要素の現在値を value に置き換える。差分 = value - GetPoint(i) を Add。
    void Set(std::size_t i, float value) noexcept
    {
        assert(i < tree_.size());
        const float diff = value - GetPoint(i);
        AddInternal(i, diff);
    }

    // i 番目要素に diff を加算する。
    void Add(std::size_t i, float diff) noexcept
    {
        assert(i < tree_.size());
        AddInternal(i, diff);
    }

    // [0, end) の和。end == 0 なら 0、end == size() なら全合計。
    float PrefixSum(std::size_t end) const noexcept
    {
        assert(end <= tree_.size());
        float s = 0.0f;
        for (std::size_t k = end; k > 0; k -= k & (~k + 1)) {
            s += tree_[k - 1];
        }
        return s;
    }

    // [from, to) の和。from > to は UB。
    float RangeSum(std::size_t from, std::size_t to) const noexcept
    {
        assert(from <= to && to <= tree_.size());
        return PrefixSum(to) - PrefixSum(from);
    }

    // 単一要素 i の値。RangeSum(i, i+1) と等価。
    float GetPoint(std::size_t i) const noexcept
    {
        assert(i < tree_.size());
        return RangeSum(i, i + 1);
    }

    // PrefixSum(i+1) > target を満たす最小の i を O(log N) で返す。
    // 全要素が非負であることを前提とする (LayoutCache の block_height = sa+h+sb >= 0)。
    // target >= PrefixSum(N) なら size() を返す (該当なし)。
    // 用途: 「累積高さ target を初めて超えるノード」の二分探索 (FindFirstVisibleNodeIndex 相当)。
    std::size_t FindIndexLowerBound(float target) const noexcept
    {
        const std::size_t n = tree_.size();
        if (n == 0) {
            return 0;
        }
        std::size_t idx = 0;
        float cum = 0.0f;
        for (std::size_t step = std::bit_floor(n); step > 0; step >>= 1) {
            const std::size_t next = idx + step;
            if (next <= n && cum + tree_[next - 1] <= target) {
                idx = next;
                cum += tree_[idx - 1];
            }
        }
        return idx;
    }

    // values をそのまま個別要素値として一括ロードする。O(N)。
    // Resize 後の初期化や、全件再構築時に Set ループ (O(N log N)) より高速。
    // values.size() == size() を要求 (Resize は呼び出し側で済ませる)。再 allocation を
    // 起こさないため noexcept。
    void Build(std::span<const float> values) noexcept
    {
        assert(values.size() == tree_.size());
        const std::size_t n = tree_.size();
        if (n > 0) {
            std::copy_n(values.data(), n, tree_.data());
        }
        for (std::size_t i = 1; i <= n; ++i) {
            const std::size_t parent = i + (i & (~i + 1));
            if (parent <= n) {
                tree_[parent - 1] += tree_[i - 1];
            }
        }
    }

private:
    void AddInternal(std::size_t i, float diff) noexcept
    {
        // 完全に 0 の差分のみ early-return。誤差で 0 にならない場合は通常の Add パスに落ちて
        // 正しさが保たれるため、FP 等値比較で問題ない (-0.0f == 0.0f も true)。
        if (diff == 0.0f) {
            return;
        }
        const std::size_t n = tree_.size();
        for (std::size_t k = i + 1; k <= n; k += k & (~k + 1)) {
            tree_[k - 1] += diff;
        }
    }

    std::pmr::vector<float> tree_;
};

} // namespace mendo
