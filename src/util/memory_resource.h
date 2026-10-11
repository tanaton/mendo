#pragma once
#include <memory_resource>

// スレッドローカル unsynchronized_pool_resource。
// allocate と deallocate を同じスレッドで行う前提。
// thread 跨ぎでメモリを引き渡してはならない。
inline std::pmr::memory_resource* GetThreadLocalPoolResource()
{
    thread_local std::pmr::unsynchronized_pool_resource pool{ std::pmr::pool_options{ /*max_blocks_per_chunk=*/0, /*largest_required_pool_block=*/1 << 20 }, std::pmr::new_delete_resource() };
    return &pool;
}
