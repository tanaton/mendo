#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <initializer_list>
#include <limits>
#include <new>
#include <type_traits>
#include <utility>

// 小さな inline 領域 (SBO) を持つ簡易 vector。
//
// 想定用途: Node::runs / TableCell::runs。Markdown ノードの大半は run 数 < 4 で、
// SBO で済めば動的確保ゼロになる。Node が default-allocator pmr::vector<TextRun> を
// 抱えていた構成では、ノード生成のたびに synchronized_pool_resource にロックを
// 取りに行く reserve(2)/reserve(8) が走っていた。SBO を使えばこれを完全に消せる。
//
// 制約 (簡略実装):
//   - T は trivially copyable / trivially destructible 限定。TextRun は POD なので OK。
//   - allocator は固定 (operator new / delete)。pmr アロケータ非対応。
//     SBO を超えた成長時のみ純 OS heap (new_delete_resource 相当) を使う。
//   - 例外保証: emplace_back / reserve は operator new 例外を伝播。SBO 範囲は noexcept。
//
// 不変式: capacity_ == N <=> data_ == inline_storage_。
namespace mendo {

template <typename T, std::size_t N>
class small_vector {
    static_assert(std::is_trivially_copyable_v<T>, "small_vector requires trivially copyable T");
    static_assert(std::is_trivially_destructible_v<T>, "small_vector requires trivially destructible T");
    static_assert(N > 0, "small_vector inline capacity must be > 0");

public:
    using value_type = T;
    using size_type = std::uint32_t;
    using iterator = T*;
    using const_iterator = const T*;
    using reference = T&;
    using const_reference = const T&;

    small_vector() noexcept = default;

    small_vector(const small_vector& other)
    {
        copy_from(other);
    }

    small_vector(small_vector&& other) noexcept
    {
        move_from(std::move(other));
    }

    small_vector& operator=(const small_vector& other)
    {
        if (this != &other) {
            release();
            copy_from(other);
        }
        return *this;
    }

    small_vector& operator=(small_vector&& other) noexcept
    {
        if (this != &other) {
            release();
            move_from(std::move(other));
        }
        return *this;
    }

    small_vector& operator=(std::initializer_list<T> il)
    {
        clear();
        const auto n = static_cast<size_type>(il.size());
        if (n > capacity_) {
            grow_to(n);
        }
        size_type i = 0;
        for (const T& v : il) {
            data_[i++] = v;
        }
        size_ = n;
        return *this;
    }

    ~small_vector()
    {
        release();
    }

    T* data() noexcept
    {
        return data_;
    }
    const T* data() const noexcept
    {
        return data_;
    }
    size_type size() const noexcept
    {
        return size_;
    }
    size_type capacity() const noexcept
    {
        return capacity_;
    }
    bool empty() const noexcept
    {
        return size_ == 0;
    }

    iterator begin() noexcept
    {
        return data_;
    }
    iterator end() noexcept
    {
        return data_ + size_;
    }
    const_iterator begin() const noexcept
    {
        return data_;
    }
    const_iterator end() const noexcept
    {
        return data_ + size_;
    }
    const_iterator cbegin() const noexcept
    {
        return data_;
    }
    const_iterator cend() const noexcept
    {
        return data_ + size_;
    }

    // 添字は size_t で受ける (size_type=uint32_t に絞ると C4267 警告の連鎖になる)。
    reference operator[](std::size_t i) noexcept
    {
        return data_[i];
    }
    const_reference operator[](std::size_t i) const noexcept
    {
        return data_[i];
    }
    reference back() noexcept
    {
        return data_[size_ - 1];
    }
    const_reference back() const noexcept
    {
        return data_[size_ - 1];
    }

    void clear() noexcept
    {
        size_ = 0;
    }

    void reserve(size_type n)
    {
        if (n > capacity_) {
            grow_to(n);
        }
    }

    template <typename... Args>
    reference emplace_back(Args&&... args)
    {
        if (size_ == capacity_) [[unlikely]] {
            grow_to(next_capacity());
        }
        T* const p = data_ + size_;
        ::new (static_cast<void*>(p)) T(std::forward<Args>(args)...);
        ++size_;
        return *p;
    }

    void push_back(const T& v)
    {
        emplace_back(v);
    }
    void push_back(T&& v)
    {
        emplace_back(std::move(v));
    }

private:
    size_type next_capacity() const noexcept
    {
        // 倍々成長。capacity_*2 が uint32 を溢れたら UINT32_MAX を渡し、
        // grow_to の operator new に bad_alloc を投げさせる (黙って縮退しない)。
        const size_type cap2 = capacity_ * 2;
        if (cap2 > capacity_) {
            return cap2;
        }
        return std::numeric_limits<size_type>::max();
    }

    static T* allocate(size_type n)
    {
        // T は trivially copyable / destructible 前提。default ctor の呼び出しを
        // 省くため operator new で生バッファを確保し、書き込み側で初期化する。
        return static_cast<T*>(::operator new(sizeof(T) * n));
    }

    static void deallocate(T* p) noexcept
    {
        ::operator delete(p);
    }

    void grow_to(size_type new_cap)
    {
        T* const new_data = allocate(new_cap);
        if (size_ > 0) {
            std::memcpy(new_data, data_, sizeof(T) * size_);
        }
        if (capacity_ > N) {
            deallocate(data_);
        }
        data_ = new_data;
        capacity_ = new_cap;
    }

    void release() noexcept
    {
        if (capacity_ > N) {
            deallocate(data_);
        }
        data_ = inline_storage_;
        capacity_ = static_cast<size_type>(N);
        size_ = 0;
    }

    void copy_from(const small_vector& other)
    {
        if (other.size_ > N) {
            data_ = allocate(other.size_);
            capacity_ = other.size_;
        }
        else {
            data_ = inline_storage_;
            capacity_ = static_cast<size_type>(N);
        }
        if (other.size_ > 0) {
            std::memcpy(data_, other.data_, sizeof(T) * other.size_);
        }
        size_ = other.size_;
    }

    // release() 後 (data_ == inline_storage_, size_=0, capacity_=N) を前提に other を引き取る。
    void move_from(small_vector&& other) noexcept
    {
        if (other.capacity_ > N) {
            data_ = other.data_;
            size_ = other.size_;
            capacity_ = other.capacity_;
            other.data_ = other.inline_storage_;
            other.size_ = 0;
            other.capacity_ = static_cast<size_type>(N);
        }
        else {
            if (other.size_ > 0) {
                std::memcpy(inline_storage_, other.data_, sizeof(T) * other.size_);
            }
            size_ = other.size_;
            other.size_ = 0;
        }
    }

    // SBO は T の default-member-init を尊重して値初期化する。TextRun の link_url_index=-1 等が
    // 走るので、SBO に未書き込みのまま読まれても定義済み値になる。
    T inline_storage_[N]{};
    T* data_ = inline_storage_;
    size_type size_ = 0;
    size_type capacity_ = static_cast<size_type>(N);
};

} // namespace mendo
