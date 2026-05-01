#pragma once
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <initializer_list>
#include <iterator>
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
//   - allocator は固定 (operator new[] / delete[])。pmr アロケータ非対応。
//     SBO を超えた成長時のみ純 OS heap (new_delete_resource 相当) を使う。
//   - 例外保証: emplace_back / reserve は new[] 例外を伝播。SBO 範囲は noexcept。
//
// レイアウト (sizeof は N=4, T=12B 想定で 64B):
//   T inline_storage_[N];   // SBO バッファ
//   T* data_;               // 実データ先頭 (inline 中は inline_storage_、heap 後は heap)
//   uint32_t size_;
//   uint32_t capacity_;
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

    small_vector(std::initializer_list<T> il)
    {
        const auto n = static_cast<size_type>(il.size());
        reserve(n);
        size_type i = 0;
        for (const T& v : il) {
            data_[i++] = v;
        }
        size_ = n;
    }

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
        reserve(n);
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

    // 添字は size_t で受ける (テスト/呼び出し側で size_t ループを書くのが自然なので、
    // ここで size_type=uint32_t に絞ると C4267 警告の連鎖になる)。
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
    reference front() noexcept
    {
        return data_[0];
    }
    const_reference front() const noexcept
    {
        return data_[0];
    }

    void clear() noexcept
    {
        // T は trivially destructible 前提なので size を戻すだけ
        size_ = 0;
    }

    void reserve(size_type n)
    {
        if (n > capacity_) {
            grow_to(n);
        }
    }

    void resize(size_type n)
    {
        if (n > capacity_) {
            grow_to(n);
        }
        // T は trivial で、新規領域を初期化しない (push_back で書き込まれる前提)
        size_ = n;
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
    void pop_back() noexcept
    {
        --size_;
    }

private:
    size_type next_capacity() const noexcept
    {
        // 倍々成長。初回は最低 N から。
        const size_type cap2 = capacity_ * 2;
        return cap2 > capacity_ ? cap2 : static_cast<size_type>(N + 1);
    }

    void grow_to(size_type new_cap)
    {
        // new_cap > N が常に成立 (capacity_ >= N で n > capacity_ なので n > N)
        T* const new_data = new T[new_cap];
        for (size_type i = 0; i < size_; ++i) {
            new_data[i] = data_[i];
        }
        if (capacity_ > N) {
            delete[] data_;
        }
        data_ = new_data;
        capacity_ = new_cap;
    }

    void release() noexcept
    {
        if (capacity_ > N) {
            delete[] data_;
        }
        data_ = inline_storage_;
        capacity_ = static_cast<size_type>(N);
        size_ = 0;
    }

    void copy_from(const small_vector& other)
    {
        if (other.size_ > N) {
            data_ = new T[other.size_];
            capacity_ = other.size_;
        }
        else {
            data_ = inline_storage_;
            capacity_ = static_cast<size_type>(N);
        }
        for (size_type i = 0; i < other.size_; ++i) {
            data_[i] = other.data_[i];
        }
        size_ = other.size_;
    }

    // release() 後 (data_ == inline_storage_, size_=0, capacity_=N) を前提に other を引き取る。
    void move_from(small_vector&& other) noexcept
    {
        if (other.capacity_ > N) {
            // other が heap を持っていた場合、ポインタごと引き取れる (浅いコピー)
            data_ = other.data_;
            size_ = other.size_;
            capacity_ = other.capacity_;
            other.data_ = other.inline_storage_;
            other.size_ = 0;
            other.capacity_ = static_cast<size_type>(N);
        }
        else {
            // other は SBO 内 → 要素を inline_storage_ にコピー
            for (size_type i = 0; i < other.size_; ++i) {
                inline_storage_[i] = other.data_[i];
            }
            size_ = other.size_;
            other.size_ = 0;
        }
    }

    // SBO は trivially copyable 前提で値初期化する (T の default ctor が走るが noop に最適化される)。
    T inline_storage_[N]{};
    T* data_ = inline_storage_;
    size_type size_ = 0;
    size_type capacity_ = static_cast<size_type>(N);
};

} // namespace mendo
