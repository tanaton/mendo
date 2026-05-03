#pragma once
#include <memory_resource>
#include <cstddef>
#include <memory>

// アプリケーション全体のメモリリソース管理
//
// デフォルトに synchronized_pool_resource を設定し、
// 必要に応じて monotonic_buffer_resource 等の特殊リソースを利用する。

// グローバル同期プールリソースの初期化。
// main() の最初に一度だけ呼び出すこと。
// 以降、std::pmr コンテナのデフォルトリソースとして使われる。
inline std::pmr::synchronized_pool_resource& GetGlobalPoolResource()
{
    // プールオプション: 最大ブロックサイズ 1MB、チャンク成長率はデフォルト
    static std::pmr::pool_options opts{ /*max_blocks_per_chunk=*/0, /*largest_required_pool_block=*/1 << 20 };
    static std::pmr::synchronized_pool_resource pool{ opts, std::pmr::new_delete_resource() };
    return pool;
}

inline void InitGlobalMemoryResource()
{
    std::pmr::set_default_resource(&GetGlobalPoolResource());
}

// ヒープ上のバッファを使う monotonic_buffer_resource のラッパー。
class MonotonicResource {
public:
    explicit MonotonicResource(std::size_t initial_size = 16 * 1024)
        // make_unique<T[]> は値初期化で N バイトを memset するため、
        // monotonic 用のスクラッチには make_unique_for_overwrite で default 初期化させる。
        : buffer_(std::make_unique_for_overwrite<std::byte[]>(initial_size)), monotonic_(buffer_.get(), initial_size, std::pmr::get_default_resource())
    {
    }

    MonotonicResource(const MonotonicResource&) = delete;
    MonotonicResource& operator=(const MonotonicResource&) = delete;

    std::pmr::memory_resource* resource() noexcept
    {
        return &monotonic_;
    }

    // 確保済みメモリを再利用可能な状態にリセットする
    void Reset() noexcept
    {
        monotonic_.release();
    }

private:
    // buffer_ は monotonic_ より先に宣言し、monotonic_ より後に破棄されるようにする
    std::unique_ptr<std::byte[]> buffer_;
    std::pmr::monotonic_buffer_resource monotonic_;
};
