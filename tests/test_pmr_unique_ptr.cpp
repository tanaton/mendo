#include "pmr_unique_ptr.h"
#include <gtest/gtest.h>
#include <atomic>
#include <memory>
#include <memory_resource>
#include <utility>

using mendo::MakePmrUnique;
using mendo::PmrDefaultDeleter;
using mendo::pmr_unique_ptr;

namespace {

// 確保/解放回数を観測するための memory_resource。
// scoped_default_resource と組み合わせて、MakePmrUnique がプロセスデフォルトを正しく
// 経由しているか / 解放が同じ resource にぶら下がるかを確かめる。
class CountingResource : public std::pmr::memory_resource {
public:
    std::atomic<size_t> alloc_count{ 0 };
    std::atomic<size_t> dealloc_count{ 0 };
    std::atomic<size_t> live_bytes{ 0 };

private:
    void* do_allocate(size_t bytes, size_t alignment) override
    {
        alloc_count.fetch_add(1, std::memory_order_relaxed);
        live_bytes.fetch_add(bytes, std::memory_order_relaxed);
        return std::pmr::new_delete_resource()->allocate(bytes, alignment);
    }
    void do_deallocate(void* p, size_t bytes, size_t alignment) override
    {
        dealloc_count.fetch_add(1, std::memory_order_relaxed);
        live_bytes.fetch_sub(bytes, std::memory_order_relaxed);
        std::pmr::new_delete_resource()->deallocate(p, bytes, alignment);
    }
    bool do_is_equal(const std::pmr::memory_resource& other) const noexcept override
    {
        return this == &other;
    }
};

class ScopedDefaultResource {
public:
    explicit ScopedDefaultResource(std::pmr::memory_resource* mr) noexcept
        : prev_(std::pmr::set_default_resource(mr))
    {
    }
    ~ScopedDefaultResource() noexcept { std::pmr::set_default_resource(prev_); }
    ScopedDefaultResource(const ScopedDefaultResource&) = delete;
    ScopedDefaultResource& operator=(const ScopedDefaultResource&) = delete;

private:
    std::pmr::memory_resource* prev_;
};

struct CtorTracker {
    inline static int alive = 0;
    int value;
    explicit CtorTracker(int v) : value{ v } { ++alive; }
    ~CtorTracker() { --alive; }
    CtorTracker(const CtorTracker&) = delete;
    CtorTracker& operator=(const CtorTracker&) = delete;
};

struct ThrowingCtor {
    inline static int alive = 0;
    explicit ThrowingCtor(bool should_throw)
    {
        if (should_throw) {
            throw std::runtime_error{ "ctor failure" };
        }
        ++alive;
    }
    ~ThrowingCtor() { --alive; }
};

} // namespace

TEST(PmrUniquePtr, EboPreservesSize)
{
    static_assert(sizeof(pmr_unique_ptr<int>) == sizeof(std::unique_ptr<int>));
    static_assert(sizeof(pmr_unique_ptr<CtorTracker>) == sizeof(void*));
}

TEST(PmrUniquePtr, MakeAllocatesAndConstructs)
{
    CountingResource mr;
    ScopedDefaultResource guard{ &mr };

    {
        auto p = MakePmrUnique<CtorTracker>(42);
        ASSERT_TRUE(p);
        EXPECT_EQ(p->value, 42);
        EXPECT_EQ(CtorTracker::alive, 1);
        EXPECT_EQ(mr.alloc_count.load(), 1u);
        EXPECT_EQ(mr.dealloc_count.load(), 0u);
    }

    EXPECT_EQ(CtorTracker::alive, 0);
    EXPECT_EQ(mr.alloc_count.load(), 1u);
    EXPECT_EQ(mr.dealloc_count.load(), 1u);
    EXPECT_EQ(mr.live_bytes.load(), 0u);
}

TEST(PmrUniquePtr, ResetReleasesViaSameResource)
{
    CountingResource mr;
    ScopedDefaultResource guard{ &mr };

    auto p = MakePmrUnique<CtorTracker>(7);
    EXPECT_EQ(mr.alloc_count.load(), 1u);
    p.reset();
    EXPECT_FALSE(p);
    EXPECT_EQ(CtorTracker::alive, 0);
    EXPECT_EQ(mr.dealloc_count.load(), 1u);
    EXPECT_EQ(mr.live_bytes.load(), 0u);
}

TEST(PmrUniquePtr, NullDeleterIsNoop)
{
    CountingResource mr;
    ScopedDefaultResource guard{ &mr };

    pmr_unique_ptr<CtorTracker> p;
    EXPECT_FALSE(p);
    p.reset();
    EXPECT_EQ(mr.alloc_count.load(), 0u);
    EXPECT_EQ(mr.dealloc_count.load(), 0u);
}

TEST(PmrUniquePtr, MoveTransfersOwnership)
{
    CountingResource mr;
    ScopedDefaultResource guard{ &mr };

    auto p1 = MakePmrUnique<CtorTracker>(1);
    auto* raw = p1.get();
    auto p2 = std::move(p1);
    EXPECT_FALSE(p1);
    EXPECT_EQ(p2.get(), raw);
    EXPECT_EQ(mr.dealloc_count.load(), 0u);
    EXPECT_EQ(CtorTracker::alive, 1);

    p2.reset();
    EXPECT_EQ(mr.dealloc_count.load(), 1u);
}

TEST(PmrUniquePtr, ThrowingCtorReleasesAllocation)
{
    CountingResource mr;
    ScopedDefaultResource guard{ &mr };

    EXPECT_THROW({ (void)MakePmrUnique<ThrowingCtor>(true); }, std::runtime_error);
    EXPECT_EQ(ThrowingCtor::alive, 0);
    EXPECT_EQ(mr.alloc_count.load(), 1u);
    EXPECT_EQ(mr.dealloc_count.load(), 1u);
    EXPECT_EQ(mr.live_bytes.load(), 0u);
}

TEST(PmrUniquePtr, RoutesViaCurrentDefaultResource)
{
    CountingResource mr_a;
    CountingResource mr_b;

    {
        ScopedDefaultResource ga{ &mr_a };
        auto p = MakePmrUnique<CtorTracker>(99);
        EXPECT_EQ(mr_a.alloc_count.load(), 1u);
        EXPECT_EQ(mr_b.alloc_count.load(), 0u);
        // p の解放は ga スコープ内 = default_resource = mr_a を経由する。
    }
    EXPECT_EQ(mr_a.dealloc_count.load(), 1u);
    EXPECT_EQ(mr_b.dealloc_count.load(), 0u);
}

TEST(PmrUniquePtr, InteropWithPmrVector)
{
    CountingResource mr;
    ScopedDefaultResource guard{ &mr };

    auto p = MakePmrUnique<std::pmr::vector<int>>();
    ASSERT_TRUE(p);
    p->push_back(1);
    p->push_back(2);
    EXPECT_EQ(p->size(), 2u);
    p.reset();

    EXPECT_EQ(mr.live_bytes.load(), 0u);
}
