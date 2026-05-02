#pragma once
#include <memory>
#include <memory_resource>
#include <type_traits>
#include <utility>

namespace mendo {

// std::pmr::get_default_resource() を使うステートレスな unique_ptr 用 deleter。
//
// Why: Node / NodeLayoutEntry が抱える unique_ptr は make_unique<T>() 経由で
// operator new に流れていた。Document 全体の pmr::vector<Node> は global
// synchronized_pool 上にあるので、子要素 (NodeTableData / TableLayoutData 等)
// も同じプールに載せると割当源を一本化できる。プールは同サイズ要求を freelist で
// 再利用するため、ドキュメント再ロード時の short-lived な小さな確保が OS heap を
// 経由しなくなる。
//
// 制約:
//   - default_resource が allocate / deallocate の間で変更されないこと。
//     mendo は main() の InitGlobalMemoryResource() でしか set_default_resource を
//     呼ばないので安全。テストは default のまま (new_delete_resource) なので、
//     allocate と deallocate が同じ resource で対称になる。
//   - Deleter はステートレス。EBO により unique_ptr のサイズはポインタ分に保たれる。
template <class T>
struct PmrDefaultDeleter {
    static_assert(!std::is_array_v<T>, "pmr_unique_ptr does not support T[]");

    static void operator()(T* p) noexcept
    {
        if (!p) {
            return;
        }
        std::pmr::polymorphic_allocator<>{ std::pmr::get_default_resource() }.delete_object(p);
    }
};

template <class T>
using pmr_unique_ptr = std::unique_ptr<T, PmrDefaultDeleter<T>>;

// pmr default_resource から T を構築する unique_ptr ファクトリ。
template <class T, class... Args>
[[nodiscard]] pmr_unique_ptr<T> MakePmrUnique(Args&&... args)
{
    std::pmr::polymorphic_allocator<> alloc{ std::pmr::get_default_resource() };
    return pmr_unique_ptr<T>{ alloc.new_object<T>(std::forward<Args>(args)...) };
}

static_assert(sizeof(pmr_unique_ptr<int>) == sizeof(int*), "pmr_unique_ptr should remain pointer-sized via EBO");

} // namespace mendo
