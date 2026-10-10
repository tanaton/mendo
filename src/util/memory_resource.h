#pragma once
#include <cstddef>
#include <memory>
#include <memory_resource>

// ヒープ上のバッファを使う monotonic_buffer_resource のラッパー。
class MonotonicResource {
public:
    explicit MonotonicResource(std::size_t initial_size = 16 * 1024)
        // make_unique_for_overwrite で値初期化の memset を回避。
        : buffer_(std::make_unique_for_overwrite<std::byte[]>(initial_size)), monotonic_(buffer_.get(), initial_size, std::pmr::new_delete_resource())
    {
    }

    MonotonicResource(const MonotonicResource&) = delete;
    MonotonicResource& operator=(const MonotonicResource&) = delete;

    std::pmr::memory_resource* resource() noexcept
    {
        return &monotonic_;
    }

    void Reset() noexcept
    {
        monotonic_.release();
    }

private:
    // buffer_ は monotonic_ より先に宣言し、monotonic_ より後に破棄されるようにする
    std::unique_ptr<std::byte[]> buffer_;
    std::pmr::monotonic_buffer_resource monotonic_;
};

// スレッドローカル unsynchronized_pool_resource。
// allocate と deallocate を同じスレッドで行う前提。
// thread 跨ぎでメモリを引き渡してはならない。
inline std::pmr::memory_resource* GetThreadLocalPoolResource()
{
    thread_local std::pmr::unsynchronized_pool_resource pool{ std::pmr::pool_options{ /*max_blocks_per_chunk=*/0, /*largest_required_pool_block=*/1 << 20 }, std::pmr::new_delete_resource() };
    return &pool;
}
