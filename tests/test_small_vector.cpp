#include "small_vector.h"
#include <gtest/gtest.h>
#include <cstdint>
#include <numeric>
#include <type_traits>

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
