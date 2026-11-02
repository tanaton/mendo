#pragma once
#include <algorithm>
#include <bit>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <format>
#include <functional>
#include <iterator>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

// 確保した後で書き込んで埋めるバイト列 (ファイルの読み込み・WebView2 の出力)。std::vector と違い確保時に 0 で埋めない。
// 埋める分は読み込みの 2 割弱にあたる (温まったキャッシュから 50MB を読んで約 2.5ms / 15ms)。
class ByteBuffer {
public:
    ByteBuffer() noexcept = default;
    // 中身は未初期化。呼び出し側が size バイトを書き込む。
    explicit ByteBuffer(size_t size) : data_(std::make_unique_for_overwrite<uint8_t[]>(size)), size_(size)
    {}
    ByteBuffer(std::span<const uint8_t> bytes) : ByteBuffer(bytes.size())
    {
        std::ranges::copy(bytes, data_.get());
    }

    uint8_t* data() noexcept
    {
        return data_.get();
    }
    const uint8_t* data() const noexcept
    {
        return data_.get();
    }
    size_t size() const noexcept
    {
        return size_;
    }
    bool empty() const noexcept
    {
        return size_ == 0;
    }
    const uint8_t* begin() const noexcept
    {
        return data_.get();
    }
    const uint8_t* end() const noexcept
    {
        return data_.get() + size_;
    }

private:
    std::unique_ptr<uint8_t[]> data_;
    size_t size_ = 0;
};

// エンコード済み PNG。図のビットマップ・ディスクキャッシュ・クリップボードで共有する。
using PngBytes = std::shared_ptr<const ByteBuffer>;

// UI スレッドで溜まった結果を GPU へ転送する 1 回の持ち時間。1 件で数 ms かかるので、まとめて届くと 1 フレームを押し出す。
inline constexpr std::chrono::microseconds kUploadSliceBudget{ 4000 };

template <class... Ts>
struct overloaded : Ts... {
    using Ts::operator()...;
};

inline constexpr std::uint64_t kFnv1a64OffsetBasis = 14695981039346656037ULL;
inline constexpr std::uint64_t kFnv1a64Prime = 1099511628211ULL;
// 2^64 / 黄金比。ハッシュに別の値を混ぜる定数。ディスクキャッシュのキーに入るので変えない。
inline constexpr std::uint64_t kGoldenRatio64 = 0x9E3779B97F4A7C15ULL;

// 短い文字列に対して衝突分布が良好な 64bit ハッシュ。
// N=10^4 規模で衝突確率 ~10^-12 と実質ゼロ。
constexpr std::uint64_t Fnv1a64(std::string_view sv) noexcept
{
    std::uint64_t h = kFnv1a64OffsetBasis;
    for (const char c : sv) {
        h = (h ^ static_cast<unsigned char>(c)) * kFnv1a64Prime;
    }
    return h;
}

// 文字列キーの unordered_map を string_view で引いてもキーを確保しないための透過ハッシュ。
// std::equal_to<> と組み合わせて使う。
struct StringTransparentHash {
    using is_transparent = void;
    static size_t operator()(std::string_view sv) noexcept
    {
        return std::hash<std::string_view>{}(sv);
    }
};

// ハッシュ値をキーにする開番地法の表。要素を 1 本の配列に並べ、unordered_map のように要素ごとに確保しない。
// 挿入で要素が動くので、ポインタは次の挿入まで有効。
template <class V>
class FlatHashMap {
public:
    size_t size() const noexcept
    {
        return size_;
    }

    const V* Find(uint64_t key) const noexcept
    {
        if (size_ == 0) {
            return nullptr;
        }
        const Slot& slot = slots_[Probe(key)];
        return slot.used ? &slot.value : nullptr;
    }

    // key が無ければ既定値で足す。second は足したとき true。
    std::pair<V*, bool> TryEmplace(uint64_t key)
    {
        // 負荷率は 1/2 まで。
        if ((size_ + 1) * 2 > slots_.size()) {
            Rebuild(std::max<size_t>(slots_.size() * 2, kMinSlots));
        }
        Slot& slot = slots_[Probe(key)];
        const bool added = !slot.used;
        if (added) {
            slot.key = key;
            slot.used = true;
            ++size_;
        }
        return { &slot.value, added };
    }

    template <class Pred>
    void EraseIf(Pred&& pred)
    {
        for (Slot& slot : slots_) {
            slot.used = slot.used && !pred(slot.value);
        }
        Rebuild(slots_.size());
    }

    // 配列は残す。
    void Clear() noexcept
    {
        std::ranges::fill(slots_, Slot{});
        size_ = 0;
    }

private:
    struct Slot {
        uint64_t key = 0;
        V value{};
        bool used = false;
    };
    static constexpr size_t kMinSlots = 16;

    // 連番に近いキーでも散らばるよう、掛けた結果の上位のビットを使う。
    size_t Home(uint64_t key) const noexcept
    {
        return static_cast<size_t>((key * kGoldenRatio64) >> std::countl_zero(slots_.size() - 1));
    }
    // key の要素か、key を置く空きの位置。
    size_t Probe(uint64_t key) const noexcept
    {
        const size_t mask = slots_.size() - 1;
        size_t i = Home(key);
        while (slots_[i].used && slots_[i].key != key) {
            i = (i + 1) & mask;
        }
        return i;
    }
    // slot_count は 2 のべき乗か 0。
    void Rebuild(size_t slot_count)
    {
        auto old = std::exchange(slots_, std::vector<Slot>(slot_count));
        size_ = 0;
        for (Slot& slot : old) {
            if (slot.used) {
                *TryEmplace(slot.key).first = std::move(slot.value);
            }
        }
    }

    std::vector<Slot> slots_;
    size_t size_ = 0;
};

// inner が outer の範囲に収まるか。md4c が渡す静的リテラルのように別の配列を指しうるポインタ同士は、
// 減算が UB で大小比較 (std::less<T*> を含む) もアドレスの数値順と一致する保証が無いため、整数で比べる。
inline bool IsSubrangeOf(std::string_view inner, std::string_view outer) noexcept
{
    const auto p = reinterpret_cast<uintptr_t>(inner.data());
    const auto base = reinterpret_cast<uintptr_t>(outer.data());
    return p >= base && p + inner.size() <= base + outer.size();
}

// スコープ離脱時にクリーンアップ関数を一度だけ実行する汎用 RAII ガード。
// デストラクタのためだけの構造体をその場で定義する代わりに使う。
// prvalue 初期化 (保証付きコピー省略) 専用のためコピー/ムーブは持たない。
template <typename F>
class ScopeGuard {
public:
    explicit ScopeGuard(F f) noexcept(std::is_nothrow_move_constructible_v<F>)
        : f_(std::move(f))
    {}
    ~ScopeGuard()
    {
        f_();
    }
    ScopeGuard(const ScopeGuard&) = delete;
    ScopeGuard& operator=(const ScopeGuard&) = delete;

private:
    F f_;
};

template <typename F>
ScopeGuard(F) -> ScopeGuard<F>;
