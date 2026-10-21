#pragma once
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <format>
#include <functional>
#include <iterator>
#include <memory>
#include <memory_resource>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

// エンコード済み PNG。図のビットマップ・ディスクキャッシュ・クリップボードで共有する。
using PngBytes = std::shared_ptr<const std::pmr::vector<uint8_t>>;

// UI スレッドで溜まった結果を GPU へ転送する 1 回の持ち時間。1 件で数 ms かかるので、まとめて届くと 1 フレームを押し出す。
inline constexpr std::chrono::microseconds kUploadSliceBudget{ 4000 };

// items を先頭から deadline まで apply し、処理した件数を返す。必ず進むよう 1 件目は期限を見ない。
template <class Items, class Apply>
size_t ApplyUntil(Items& items, std::chrono::steady_clock::time_point deadline, Apply&& apply)
{
    size_t done = 0;
    for (; done < items.size(); ++done) {
        if (done > 0 && std::chrono::steady_clock::now() >= deadline) {
            break;
        }
        apply(items[done]);
    }
    return done;
}

// ApplyUntil で残った items[done..] を queue の先頭へ戻す。
template <class Queue, class Items>
void RequeueFront(Queue& queue, Items& items, size_t done)
{
    const auto rest = items.begin() + static_cast<std::ptrdiff_t>(done);
    queue.insert(queue.begin(), std::make_move_iterator(rest), std::make_move_iterator(items.end()));
    items.erase(rest, items.end());
}

template <class... Ts>
struct overloaded : Ts... {
    using Ts::operator()...;
};

inline constexpr std::uint64_t kFnv1a64OffsetBasis = 14695981039346656037ULL;
inline constexpr std::uint64_t kFnv1a64Prime = 1099511628211ULL;

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

// inner が outer の範囲に収まるか。md4c が渡す静的リテラルのように別の配列を指しうるポインタ同士は、
// 減算が UB で大小比較 (std::less<T*> を含む) もアドレスの数値順と一致する保証が無いため、整数で比べる。
inline bool IsSubrangeOf(std::string_view inner, std::string_view outer) noexcept
{
    const auto p = reinterpret_cast<uintptr_t>(inner.data());
    const auto base = reinterpret_cast<uintptr_t>(outer.data());
    return p >= base && p + inner.size() <= base + outer.size();
}

template <typename... Args>
inline std::pmr::wstring PmrFormat(std::wformat_string<Args...> fmt, Args&&... args)
{
    std::pmr::wstring result;
    std::format_to(std::back_inserter(result), fmt, std::forward<Args>(args)...);
    return result;
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
