#include "small_vector.h"
#include <gtest/gtest.h>
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <format>
#include <initializer_list>
#include <numeric>
#include <random>
#include <type_traits>
#include <utility>
#include <vector>

using mendo::small_vector;

namespace {

struct Pod {
    uint32_t a = 0;
    int16_t b = -1;
    uint8_t c = 0;
};
static_assert(std::is_trivially_copyable_v<Pod>);
static_assert(std::is_trivially_destructible_v<Pod>);

} // namespace

// ---- 基本 ----

TEST(SmallVector, DefaultConstructed)
{
    small_vector<int, 4> v;
    EXPECT_EQ(v.size(), 0u);
    EXPECT_EQ(v.capacity(), 4u);
    EXPECT_TRUE(v.empty());
    EXPECT_EQ(v.begin(), v.end());
}

TEST(SmallVector, PushBackWithinSboNoHeap)
{
    small_vector<int, 4> v;
    const int* sbo_data = v.data();
    v.push_back(1);
    v.push_back(2);
    v.push_back(3);
    v.push_back(4);
    EXPECT_EQ(v.size(), 4u);
    EXPECT_EQ(v.capacity(), 4u);
    EXPECT_EQ(v.data(), sbo_data) << "SBO 範囲では data ポインタは不変 (ヒープ確保していないことの確認)";
    EXPECT_EQ(v[0], 1);
    EXPECT_EQ(v[3], 4);
    EXPECT_EQ(v.back(), 4);
}

TEST(SmallVector, PushBackBeyondSboGrowsToHeap)
{
    small_vector<int, 4> v;
    const int* sbo_data = v.data();
    for (int i = 0; i < 4; ++i) {
        v.push_back(i);
    }
    v.push_back(99); // SBO 超え
    EXPECT_EQ(v.size(), 5u);
    EXPECT_GE(v.capacity(), 5u);
    EXPECT_NE(v.data(), sbo_data) << "SBO 超え時はヒープに移動するので data ポインタが変わる";
    EXPECT_EQ(v[0], 0);
    EXPECT_EQ(v[3], 3);
    EXPECT_EQ(v[4], 99);
}

TEST(SmallVector, GrowthDoublesCapacity)
{
    small_vector<int, 2> v;
    v.push_back(0);
    v.push_back(1);
    EXPECT_EQ(v.capacity(), 2u);
    v.push_back(2); // 2->4
    EXPECT_EQ(v.capacity(), 4u);
    v.push_back(3);
    v.push_back(4); // 4->8
    EXPECT_EQ(v.capacity(), 8u);
    for (int i = 0; i < 5; ++i) {
        EXPECT_EQ(v[i], i);
    }
}

TEST(SmallVector, ReserveGrowsCapacityOnly)
{
    small_vector<int, 2> v;
    v.push_back(7);
    v.reserve(16);
    EXPECT_EQ(v.size(), 1u);
    EXPECT_GE(v.capacity(), 16u);
    EXPECT_EQ(v[0], 7);
}

TEST(SmallVector, ReserveBelowCapacityIsNoop)
{
    small_vector<int, 4> v;
    v.push_back(1);
    const int* before = v.data();
    v.reserve(2);
    EXPECT_EQ(v.capacity(), 4u);
    EXPECT_EQ(v.data(), before);
}

TEST(SmallVector, ClearKeepsCapacity)
{
    small_vector<int, 2> v;
    for (int i = 0; i < 10; ++i) {
        v.push_back(i);
    }
    const auto cap = v.capacity();
    const int* data = v.data();
    v.clear();
    EXPECT_EQ(v.size(), 0u);
    EXPECT_TRUE(v.empty());
    EXPECT_EQ(v.capacity(), cap);
    EXPECT_EQ(v.data(), data) << "clear はヒープ領域を維持する (再利用前提)";
}

// ---- emplace_back ----

TEST(SmallVector, EmplaceBackPodReturnsRef)
{
    small_vector<Pod, 2> v;
    Pod& r = v.emplace_back(Pod{42, -7, 5});
    EXPECT_EQ(&r, &v[0]);
    EXPECT_EQ(r.a, 42u);
    EXPECT_EQ(r.b, -7);
    EXPECT_EQ(r.c, 5);
}

TEST(SmallVector, EmplaceBackPreservesValuesAcrossGrowth)
{
    small_vector<Pod, 2> v;
    for (uint32_t i = 0; i < 16; ++i) {
        v.emplace_back(Pod{i, static_cast<int16_t>(-static_cast<int>(i)), static_cast<uint8_t>(i & 0xFF)});
    }
    EXPECT_EQ(v.size(), 16u);
    for (uint32_t i = 0; i < 16; ++i) {
        EXPECT_EQ(v[i].a, i);
        EXPECT_EQ(v[i].b, -static_cast<int16_t>(i));
        EXPECT_EQ(v[i].c, static_cast<uint8_t>(i & 0xFF));
    }
}

// ---- イテレータ ----

TEST(SmallVector, IteratorTraversal)
{
    small_vector<int, 4> v;
    for (int i = 0; i < 7; ++i) {
        v.push_back(i + 1);
    }
    int sum = std::accumulate(v.begin(), v.end(), 0);
    EXPECT_EQ(sum, 1 + 2 + 3 + 4 + 5 + 6 + 7);

    const auto& cv = v;
    int csum = 0;
    for (auto it = cv.cbegin(); it != cv.cend(); ++it) {
        csum += *it;
    }
    EXPECT_EQ(csum, sum);
}

// ---- コピー ----

TEST(SmallVector, CopyConstructSboIndependent)
{
    small_vector<int, 4> a;
    a.push_back(10);
    a.push_back(20);

    small_vector<int, 4> b(a);
    EXPECT_EQ(b.size(), 2u);
    EXPECT_EQ(b[0], 10);
    EXPECT_EQ(b[1], 20);

    // SBO なので独立した記憶領域を持つはず
    EXPECT_NE(a.data(), b.data());

    a[0] = 999;
    EXPECT_EQ(b[0], 10) << "コピー後の独立性";
}

TEST(SmallVector, CopyConstructHeap)
{
    small_vector<int, 2> a;
    for (int i = 0; i < 8; ++i) {
        a.push_back(i);
    }
    small_vector<int, 2> b(a);
    EXPECT_EQ(b.size(), 8u);
    EXPECT_GE(b.capacity(), 8u);
    EXPECT_NE(a.data(), b.data());
    for (int i = 0; i < 8; ++i) {
        EXPECT_EQ(b[i], i);
    }
}

TEST(SmallVector, CopyAssignReplacesContents)
{
    small_vector<int, 2> a;
    for (int i = 0; i < 6; ++i) {
        a.push_back(i);
    }
    small_vector<int, 2> b;
    b.push_back(100);
    b.push_back(200);
    b = a;
    EXPECT_EQ(b.size(), 6u);
    for (int i = 0; i < 6; ++i) {
        EXPECT_EQ(b[i], i);
    }
}

TEST(SmallVector, SelfCopyAssignSafe)
{
    small_vector<int, 2> a;
    for (int i = 0; i < 6; ++i) {
        a.push_back(i);
    }
    auto& ref = a;
    a = ref; // 自己代入
    EXPECT_EQ(a.size(), 6u);
    for (int i = 0; i < 6; ++i) {
        EXPECT_EQ(a[i], i);
    }
}

// ---- ムーブ ----

TEST(SmallVector, MoveConstructHeapStealsBuffer)
{
    small_vector<int, 2> a;
    for (int i = 0; i < 8; ++i) {
        a.push_back(i);
    }
    const int* heap_buf = a.data();

    small_vector<int, 2> b(std::move(a));
    EXPECT_EQ(b.size(), 8u);
    EXPECT_EQ(b.data(), heap_buf) << "ヒープ領域は所有権が移るのでアドレスは変わらない";

    EXPECT_EQ(a.size(), 0u);
    EXPECT_EQ(a.capacity(), 2u) << "ムーブ元は SBO に戻る";
}

TEST(SmallVector, MoveConstructSboCopiesContents)
{
    small_vector<int, 4> a;
    a.push_back(1);
    a.push_back(2);

    small_vector<int, 4> b(std::move(a));
    EXPECT_EQ(b.size(), 2u);
    EXPECT_EQ(b[0], 1);
    EXPECT_EQ(b[1], 2);
    EXPECT_EQ(a.size(), 0u);
}

TEST(SmallVector, MoveAssignReleasesPrevious)
{
    small_vector<int, 2> a;
    for (int i = 0; i < 8; ++i) {
        a.push_back(i);
    }
    small_vector<int, 2> b;
    for (int i = 0; i < 6; ++i) {
        b.push_back(i + 100);
    }
    b = std::move(a);
    EXPECT_EQ(b.size(), 8u);
    for (int i = 0; i < 8; ++i) {
        EXPECT_EQ(b[i], i);
    }
    EXPECT_EQ(a.size(), 0u);
}

TEST(SmallVector, SelfMoveAssignSafe)
{
    small_vector<int, 2> a;
    for (int i = 0; i < 6; ++i) {
        a.push_back(i);
    }
    auto& ref = a;
    a = std::move(ref); // 自己ムーブ
    EXPECT_EQ(a.size(), 6u);
    for (int i = 0; i < 6; ++i) {
        EXPECT_EQ(a[i], i);
    }
}

// ---- 初期化リスト代入 ----

TEST(SmallVector, AssignInitializerListWithinSbo)
{
    small_vector<int, 4> v;
    v.push_back(99); // 既存値
    v = {1, 2, 3};
    EXPECT_EQ(v.size(), 3u);
    EXPECT_EQ(v[0], 1);
    EXPECT_EQ(v[1], 2);
    EXPECT_EQ(v[2], 3);
}

TEST(SmallVector, AssignInitializerListGrowsHeap)
{
    small_vector<int, 2> v;
    v = {10, 20, 30, 40, 50};
    EXPECT_EQ(v.size(), 5u);
    EXPECT_GE(v.capacity(), 5u);
    EXPECT_EQ(v[0], 10);
    EXPECT_EQ(v[4], 50);
}

TEST(SmallVector, AssignEmptyInitializerListClears)
{
    small_vector<int, 2> v;
    v.push_back(7);
    v.push_back(8);
    v = {};
    EXPECT_EQ(v.size(), 0u);
    EXPECT_TRUE(v.empty());
}

// ---- 退避/読み取り ----

TEST(SmallVector, BackReturnsLastElement)
{
    small_vector<int, 2> v;
    v.push_back(10);
    EXPECT_EQ(v.back(), 10);
    v.push_back(20);
    EXPECT_EQ(v.back(), 20);
    v.push_back(30); // ヒープへ
    EXPECT_EQ(v.back(), 30);
}

TEST(SmallVector, ConstAccess)
{
    small_vector<int, 4> v;
    v.push_back(11);
    v.push_back(22);
    const auto& cv = v;
    EXPECT_EQ(cv[0], 11);
    EXPECT_EQ(cv.back(), 22);
    EXPECT_EQ(*cv.data(), 11);
    EXPECT_EQ(cv.size(), 2u);
}

// ---- 実用シナリオ: TextRun を入れる ----

TEST(SmallVector, RealisticTextRunUsage)
{
    struct TextRunLike {
        uint32_t start = 0;
        uint32_t length = 0;
        int16_t link_url_index = -1;
        uint8_t flags = 0;
    };
    static_assert(std::is_trivially_copyable_v<TextRunLike>);

    small_vector<TextRunLike, 4> runs;
    EXPECT_EQ(runs.capacity(), 4u);

    // 典型的な run 数 (median < 4) は SBO で済む
    runs.emplace_back(TextRunLike{0, 5, -1, 0x01});
    runs.emplace_back(TextRunLike{5, 10, 2, 0});
    EXPECT_EQ(runs.size(), 2u);
    EXPECT_EQ(runs.capacity(), 4u);
    EXPECT_EQ(runs[0].length, 5u);
    EXPECT_EQ(runs[1].link_url_index, 2);
}

// ---- N=1 / 大きい N ----

TEST(SmallVector, N1Capacity)
{
    small_vector<int, 1> v;
    EXPECT_EQ(v.capacity(), 1u);
    v.push_back(7);
    EXPECT_EQ(v.capacity(), 1u);
    v.push_back(8);
    EXPECT_GE(v.capacity(), 2u);
    EXPECT_EQ(v[0], 7);
    EXPECT_EQ(v[1], 8);
}

TEST(SmallVector, ManyElements)
{
    small_vector<int, 4> v;
    for (int i = 0; i < 1000; ++i) {
        v.push_back(i);
    }
    EXPECT_EQ(v.size(), 1000u);
    for (int i = 0; i < 1000; ++i) {
        EXPECT_EQ(v[i], i);
    }
}

// ---- 不変式: SBO 復帰 ----

TEST(SmallVector, MovedFromIsUsableAgain)
{
    small_vector<int, 2> a;
    for (int i = 0; i < 8; ++i) {
        a.push_back(i);
    }
    small_vector<int, 2> b(std::move(a));
    (void)b;
    // a はヒープを手放して SBO に戻っているので、再度使える
    a.push_back(42);
    EXPECT_EQ(a.size(), 1u);
    EXPECT_EQ(a[0], 42);
    EXPECT_EQ(a.capacity(), 2u);
}

// ---- 自己参照の push_back ----

// std::vector と同じく、自身の要素への参照を渡しても成長時に壊れない。
// 旧領域を新要素の構築前に解放すると解放済みメモリを読む。
TEST(SmallVector, PushBackOwnElementWhileGrowingFromHeap)
{
    struct Wide {
        uint64_t v[8];
    };
    for (const uint32_t initial : { 2u, 4u, 8u, 64u, 1024u }) {
        SCOPED_TRACE(initial);
        small_vector<Wide, 1> sv;
        for (uint32_t i = 0; i < initial; ++i) {
            Wide w{};
            std::ranges::fill(w.v, 0x0101010101010101ull * (i + 1));
            sv.push_back(w);
        }
        ASSERT_EQ(sv.size(), sv.capacity()) << "次の push_back で成長させる前提";
        const Wide expected = sv[0];
        sv.push_back(sv[0]);
        EXPECT_TRUE(std::ranges::equal(sv.back().v, expected.v));
    }
}

// ---- std::vector とのモデル比較 ----

namespace {

struct WideElem {
    uint64_t a = 0;
    uint64_t b = 0;
    uint64_t c = 0;
    bool operator==(const WideElem&) const = default;
};

template <typename T>
T MakeModelValue(uint32_t x)
{
    if constexpr (std::is_same_v<T, WideElem>) {
        return WideElem{ x, ~uint64_t{ x }, uint64_t{ x } * 3 };
    }
    else {
        return static_cast<T>(x);
    }
}

template <typename T, std::size_t N>
testing::AssertionResult MatchesModel(const small_vector<T, N>& v, const std::vector<T>& m)
{
    if (v.size() != m.size() || v.empty() != m.empty() || static_cast<std::size_t>(v.end() - v.begin()) != m.size()) {
        return testing::AssertionFailure() << "size " << v.size() << " vs model " << m.size();
    }
    if (v.capacity() < v.size()) {
        return testing::AssertionFailure() << "capacity " << v.capacity() << " < size " << v.size();
    }
    // 不変式: capacity == N <=> data が inline 領域 (オブジェクト内) を指す
    const auto* self = reinterpret_cast<const std::byte*>(&v);
    const auto* data = reinterpret_cast<const std::byte*>(v.data());
    const bool is_inline = data >= self && data < self + sizeof(v);
    if (is_inline != (v.capacity() == N)) {
        return testing::AssertionFailure() << "inline=" << is_inline << " capacity=" << v.capacity();
    }
    const auto mismatch = std::ranges::mismatch(v, m);
    if (mismatch.in1 != v.end()) {
        return testing::AssertionFailure() << "element mismatch at " << (mismatch.in1 - v.begin());
    }
    return testing::AssertionSuccess();
}

enum class ModelOp {
    PushBack,
    PushBackOwnElement,
    EmplaceBack,
    AssignFill,
    AssignInitList,
    Clear,
    Reserve,
    CopyAssign,
    MoveAssign,
    CopyConstruct,
    MoveConstruct,
    Count,
};

// 2 本を並走させ、片方から他方 (または自分自身) への copy/move も混ぜる。
// ムーブ元は空として再利用され続ける。
template <typename T, std::size_t N>
void RunModelSequence(uint32_t seed, int steps)
{
    std::mt19937 rng{ seed };
    const auto below = [&](std::size_t n) {
        return std::uniform_int_distribution<std::size_t>(0, n - 1)(rng);
    };
    small_vector<T, N> v[2];
    std::vector<T> m[2];
    uint32_t next = 1;
    for (int step = 0; step < steps; ++step) {
        const auto op = static_cast<ModelOp>(below(static_cast<std::size_t>(ModelOp::Count)));
        const std::size_t a = below(2);
        const std::size_t b = below(2);
        SCOPED_TRACE(std::format("N={} seed={} step={} op={} a={} b={}", N, seed, step, static_cast<int>(op), a, b));
        switch (op) {
        case ModelOp::PushBack: {
            const T x = MakeModelValue<T>(next++);
            v[a].push_back(x);
            m[a].push_back(x);
            break;
        }
        case ModelOp::PushBackOwnElement:
            if (!m[a].empty()) {
                const std::size_t i = below(m[a].size());
                v[a].push_back(v[a][i]);
                m[a].push_back(m[a][i]);
            }
            break;
        case ModelOp::EmplaceBack:
            v[a].emplace_back(MakeModelValue<T>(next));
            m[a].push_back(MakeModelValue<T>(next));
            ++next;
            break;
        case ModelOp::AssignFill: {
            const std::size_t n = below(3 * N + 2);
            const T x = MakeModelValue<T>(next++);
            v[a].assign(n, x);
            m[a].assign(n, x);
            break;
        }
        case ModelOp::AssignInitList: {
            const T x = MakeModelValue<T>(next++);
            const T y = MakeModelValue<T>(next++);
            switch (below(4)) {
            case 0:
                v[a] = std::initializer_list<T>{};
                m[a] = std::initializer_list<T>{};
                break;
            case 1:
                v[a] = { x };
                m[a] = { x };
                break;
            case 2:
                v[a] = { x, y, x };
                m[a] = { x, y, x };
                break;
            default:
                v[a] = { y, x, y, x, y, x, y, x, y };
                m[a] = { y, x, y, x, y, x, y, x, y };
                break;
            }
            break;
        }
        case ModelOp::Clear:
            v[a].clear();
            m[a].clear();
            break;
        case ModelOp::Reserve: {
            const auto n = static_cast<uint32_t>(below(4 * N + 4));
            v[a].reserve(n);
            m[a].reserve(n);
            ASSERT_GE(v[a].capacity(), n);
            break;
        }
        case ModelOp::CopyAssign:
            v[a] = v[b];
            m[a] = m[b];
            break;
        case ModelOp::MoveAssign:
            if (a == b) {
                auto& self = v[a];
                v[a] = std::move(self);
            }
            else {
                v[a] = std::move(v[b]);
                m[a] = std::move(m[b]);
                m[b].clear();
            }
            break;
        case ModelOp::CopyConstruct: {
            small_vector<T, N> c(v[b]);
            ASSERT_TRUE(MatchesModel(c, m[b]));
            v[a] = std::move(c);
            m[a] = m[b];
            break;
        }
        case ModelOp::MoveConstruct: {
            small_vector<T, N> c(std::move(v[b]));
            std::vector<T> mc = std::move(m[b]);
            m[b].clear();
            ASSERT_TRUE(MatchesModel(c, mc));
            ASSERT_TRUE(MatchesModel(v[b], m[b]));
            v[a] = std::move(c);
            m[a] = std::move(mc);
            break;
        }
        case ModelOp::Count:
            break;
        }
        ASSERT_TRUE(MatchesModel(v[0], m[0]));
        ASSERT_TRUE(MatchesModel(v[1], m[1]));
    }
}

} // namespace

TEST(SmallVector, MatchesStdVectorUnderRandomOps)
{
    for (const uint32_t seed : { 1u, 2u, 3u, 0xC0FFEEu, 20261007u }) {
        ASSERT_NO_FATAL_FAILURE((RunModelSequence<uint32_t, 1>(seed, 2000)));
        ASSERT_NO_FATAL_FAILURE((RunModelSequence<uint32_t, 2>(seed, 2000)));
        ASSERT_NO_FATAL_FAILURE((RunModelSequence<uint32_t, 4>(seed, 2000)));
        ASSERT_NO_FATAL_FAILURE((RunModelSequence<WideElem, 1>(seed, 2000)));
        ASSERT_NO_FATAL_FAILURE((RunModelSequence<WideElem, 4>(seed, 2000)));
    }
}
