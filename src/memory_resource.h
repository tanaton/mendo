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
inline std::pmr::synchronized_pool_resource& GetGlobalPoolResource() {
    // プールオプション: 最大ブロックサイズ 1MB、チャンク成長率はデフォルト
    static std::pmr::pool_options opts{/*max_blocks_per_chunk=*/0, /*largest_required_pool_block=*/1 << 20};
    static std::pmr::synchronized_pool_resource pool{opts, std::pmr::new_delete_resource()};
    return pool;
}

inline void InitGlobalMemoryResource() {
    std::pmr::set_default_resource(&GetGlobalPoolResource());
}

// スコープに紐づくヒープ上のバッファを使う monotonic_buffer_resource のラッパー。
// 一括確保→一括解放パターン（パース、描画コマンド生成など）に最適。
class ScopedMonotonicResource {
public:
    explicit ScopedMonotonicResource(std::size_t initial_size = 16 * 1024)
        : buffer_(std::make_unique<std::byte[]>(initial_size))
        , monotonic_(buffer_.get(), initial_size, std::pmr::get_default_resource())
    {}

    ScopedMonotonicResource(const ScopedMonotonicResource&) = delete;
    ScopedMonotonicResource& operator=(const ScopedMonotonicResource&) = delete;

    std::pmr::memory_resource* resource() noexcept { return &monotonic_; }

private:
    // buffer_ は monotonic_ より先に宣言し、monotonic_ より後に破棄されるようにする
    std::unique_ptr<std::byte[]> buffer_;
    std::pmr::monotonic_buffer_resource monotonic_;
};

// フレーム毎にリセット可能な monotonic_buffer_resource。
// 描画コマンドリスト等、毎フレーム一括確保→リセットするパターンに最適。
class FrameMonotonicResource {
public:
    explicit FrameMonotonicResource(std::size_t initial_size = 64 * 1024)
        : buffer_(std::make_unique<std::byte[]>(initial_size))
        , monotonic_(buffer_.get(), initial_size, std::pmr::get_default_resource())
    {}

    FrameMonotonicResource(const FrameMonotonicResource&) = delete;
    FrameMonotonicResource& operator=(const FrameMonotonicResource&) = delete;

    std::pmr::memory_resource* resource() noexcept { return &monotonic_; }

    // フレーム終了時にリセット（確保済みメモリを再利用）
    void Reset() {
        monotonic_.release();
    }

private:
    // buffer_ は monotonic_ より先に宣言し、monotonic_ より後に破棄されるようにする
    std::unique_ptr<std::byte[]> buffer_;
    std::pmr::monotonic_buffer_resource monotonic_;
};
