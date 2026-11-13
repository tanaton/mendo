#pragma once
#include <algorithm>
#include <cassert>
#include <concepts>
#include <cstddef>
#include <cstring>
#include <memory>
#include <new>
#include <ranges>
#include <span>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

// アドレス空間の予約とコミット (VirtualAlloc)。windows.h をヘッダに出さないため、定義は document.cpp。
namespace reserved_memory {
inline constexpr size_t kPageSize = 4096;
// bytes (ページの倍数) のアドレスを予約する。コミットはしない。失敗すると null。
void* Reserve(size_t bytes) noexcept;
// 予約した中の [p, p + bytes) をコミットする。p と bytes はページの倍数。
bool Commit(void* p, size_t bytes) noexcept;
// [p, p + bytes) のコミットを外す。予約は残る。
void Decommit(void* p, size_t bytes) noexcept;
// Reserve で得た予約を丸ごと返す。
void Release(void* p) noexcept;
} // namespace reserved_memory

// 要素数の上限の分のアドレスを予約し、使う分だけコミットして伸ばす配列。std::vector と違い、伸ばしても要素を写さず、
// 予約の余りはコミットしない (ノード配列は入力 128 byte に 1 個を予約してパース中に実際の約 2 倍をコミットし、
// 足りなければ全ノードを写して伸ばし、余れば写して詰めていた)。アドレスは 64bit の空間から取るので、上限は多めに決めてよい。
// 予約は丸ごとしか返せず、使わない部分のアドレスも寿命の間持つ。予約を超えたら 2 倍ずつ予約し直す (そのときだけ写す)。
// 要素は写して動かすので trivially copyable に限る。
template <class T>
    requires std::is_trivially_copyable_v<T>
class ReservedArray {
public:
    ReservedArray() noexcept = default;
    // initial 個分を予約する。
    explicit ReservedArray(size_t initial)
    {
        Relocate(initial);
    }
    ~ReservedArray()
    {
        reserved_memory::Release(data_);
    }
    ReservedArray(ReservedArray&& other) noexcept
        : data_(std::exchange(other.data_, nullptr)), size_(std::exchange(other.size_, 0)), committed_(std::exchange(other.committed_, 0)),
          reserved_(std::exchange(other.reserved_, 0))
    {
    }
    ReservedArray& operator=(ReservedArray&& other) noexcept
    {
        if (this != &other) {
            reserved_memory::Release(data_);
            data_ = std::exchange(other.data_, nullptr);
            size_ = std::exchange(other.size_, 0);
            committed_ = std::exchange(other.committed_, 0);
            reserved_ = std::exchange(other.reserved_, 0);
        }
        return *this;
    }
    ReservedArray(const ReservedArray&) = delete;
    ReservedArray& operator=(const ReservedArray&) = delete;

    size_t size() const noexcept
    {
        return size_;
    }
    bool empty() const noexcept
    {
        return size_ == 0;
    }
    // コミット済みの要素数。
    size_t capacity() const noexcept
    {
        return committed_;
    }
    T* data() noexcept
    {
        return data_;
    }
    const T* data() const noexcept
    {
        return data_;
    }
    T* begin() noexcept
    {
        return data_;
    }
    T* end() noexcept
    {
        return data_ + size_;
    }
    const T* begin() const noexcept
    {
        return data_;
    }
    const T* end() const noexcept
    {
        return data_ + size_;
    }
    T& operator[](size_t i) noexcept
    {
        assert(i < size_);
        return data_[i];
    }
    const T& operator[](size_t i) const noexcept
    {
        assert(i < size_);
        return data_[i];
    }
    T& back() noexcept
    {
        assert(size_ > 0);
        return data_[size_ - 1];
    }
    const T& back() const noexcept
    {
        assert(size_ > 0);
        return data_[size_ - 1];
    }

    template <class... Args>
    T& emplace_back(Args&&... args)
    {
        if (size_ == committed_) [[unlikely]] {
            Grow(size_ + 1);
        }
        T* const p = std::construct_at(data_ + size_, std::forward<Args>(args)...);
        ++size_;
        return *p;
    }
    void push_back(const T& value)
    {
        emplace_back(value);
    }
    void append_range(std::span<const T> items)
    {
        if (items.empty()) {
            return;
        }
        if (committed_ - size_ < items.size()) [[unlikely]] {
            Grow(size_ + items.size());
        }
        std::memcpy(data_ + size_, items.data(), items.size_bytes());
        size_ += items.size();
    }
    // pos の前に items を入れる。
    void insert_range(size_t pos, std::span<const T> items)
    {
        assert(pos <= size_);
        append_range(items);
        std::rotate(data_ + pos, data_ + size_ - items.size(), data_ + size_);
    }
    void pop_back() noexcept
    {
        assert(size_ > 0);
        --size_;
    }
    // 先頭の count 個に縮める。コミットは残すので、また伸ばすときはコミットし直さない。
    void truncate(size_t count) noexcept
    {
        assert(count <= size_);
        size_ = count;
    }
    void clear() noexcept
    {
        size_ = 0;
    }
    // 使っていない末尾のページのコミットを外す。要素は写さない。
    void shrink_to_fit() noexcept
    {
        const size_t used = RoundUpToPage(size_ * sizeof(T));
        const size_t committed = committed_ * sizeof(T);
        if (committed > used) {
            reserved_memory::Decommit(reinterpret_cast<std::byte*>(data_) + used, RoundUpToPage(committed) - used);
            committed_ = used / sizeof(T);
        }
    }

    // 文字の並びとして見る。
    operator std::string_view() const noexcept
        requires std::same_as<T, char>
    {
        return { data_, size_ };
    }
    std::string_view view() const noexcept
        requires std::same_as<T, char>
    {
        return { data_, size_ };
    }
    void append(std::string_view text)
        requires std::same_as<T, char>
    {
        append_range(std::span<const char>{ text });
    }
    void insert(size_t pos, std::string_view text)
        requires std::same_as<T, char>
    {
        insert_range(pos, std::span<const char>{ text });
    }
    friend bool operator==(const ReservedArray& a, std::string_view b) noexcept
        requires std::same_as<T, char>
    {
        return a.view() == b;
    }

private:
    static constexpr size_t kMinCommitStep = 64 * 1024;
    static constexpr size_t kMaxCommitStep = 1024 * 1024;

    static constexpr size_t RoundUpToPage(size_t bytes) noexcept
    {
        return (bytes + reserved_memory::kPageSize - 1) & ~(reserved_memory::kPageSize - 1);
    }

    // count 個が入るまでコミットを広げる。足すのは今の 1/4 で、1 回 kMinCommitStep から kMaxCommitStep まで。
    // 最大を抑えるのは、余りがパースの終わりの最大に重なるため (リンク集の 141MB のノード配列で 1/4 ずつだと最大が +1.7MB)。
    // 最小を置くのは、小さな文書で 4KB ずつ足すと VirtualAlloc の回数が増えるため。予約を超えるなら予約し直す。
    void Grow(size_t count)
    {
        if (count > reserved_) {
            Relocate(std::max(reserved_ * 2, count));
            if (count > reserved_) {
                throw std::bad_alloc();
            }
        }
        const size_t committed = committed_ * sizeof(T);
        const size_t step = std::clamp(committed_ / 4, kMinCommitStep / sizeof(T), kMaxCommitStep / sizeof(T));
        const size_t want = std::min(RoundUpToPage(std::max(count, committed_ + step) * sizeof(T)), reserved_ * sizeof(T));
        if (!reserved_memory::Commit(reinterpret_cast<std::byte*>(data_) + RoundUpToPage(committed), want - RoundUpToPage(committed))) {
            throw std::bad_alloc();
        }
        committed_ = want / sizeof(T);
    }

    // count 個分の予約を取り直し、要素を写す。予約できなければ半分ずつ減らす (32bit の空間など)。
    void Relocate(size_t count)
    {
        size_t bytes = RoundUpToPage(std::max<size_t>(count, 1) * sizeof(T));
        void* fresh = nullptr;
        while (!(fresh = reserved_memory::Reserve(bytes)) && bytes / 2 >= RoundUpToPage((size_ + 1) * sizeof(T))) {
            bytes = RoundUpToPage(bytes / 2);
        }
        if (!fresh) {
            throw std::bad_alloc();
        }
        if (data_) {
            const size_t used = RoundUpToPage(size_ * sizeof(T));
            if (used > 0 && !reserved_memory::Commit(fresh, used)) {
                reserved_memory::Release(fresh);
                throw std::bad_alloc();
            }
            std::memcpy(fresh, data_, size_ * sizeof(T));
            reserved_memory::Release(data_);
            committed_ = used / sizeof(T);
        }
        data_ = static_cast<T*>(fresh);
        reserved_ = bytes / sizeof(T);
    }

    T* data_ = nullptr;
    size_t size_ = 0;
    size_t committed_ = 0;
    size_t reserved_ = 0;
};

// パース後に書き換えないノードのテキスト・TextRun・リンク URL を置く領域。
// 個別には解放せず、Document の差し替え・破棄のときにまとめて捨てる。
// チャンクを積み足すだけのバンプアロケータ。チャンクは小さく始めて kMaxChunkSize まで倍々に広げ、
// 小さな文書で大きく確保せず、大きな文書でも使わずに残る領域を末尾のチャンク 1 個分程度に抑える。
// チャンクはヒープにあるので、移しても配ったポインタは有効なまま。移し元はもう使わないこと。
class TextArena {
public:
    std::string_view CopyText(std::string_view text)
    {
        const auto s = Copy(text);
        return { s.data(), s.size() };
    }

    // TextRun や、DocumentData::raw / arena 上の文字列を指す string_view の配列を置く。
    template <std::ranges::contiguous_range R, class T = std::ranges::range_value_t<R>>
        requires std::ranges::sized_range<R> && std::is_trivially_copyable_v<T>
    std::span<const T> Copy(const R& items)
    {
        const std::span<const T> src{ items };
        if (src.empty()) {
            return {};
        }
        void* const p = Allocate(src.size_bytes(), alignof(T));
        std::memcpy(p, src.data(), src.size_bytes());
        return { static_cast<const T*>(p), src.size() };
    }

    // 組んだ置き場を写さずに引き取る (大きな表の本文)。ReservedArray は動かしても要素の場所が変わらないので、返した view は
    // arena と同じ寿命の間有効。
    std::string_view Adopt(ReservedArray<char>&& text)
    {
        return adopted_.emplace_back(std::move(text));
    }

private:
    static constexpr size_t kMinChunkSize = 4 * 1024;
    static constexpr size_t kMaxChunkSize = 1024 * 1024;
    // これを超える要求は専用に確保する。チャンク末尾の使い残しを 1/4 チャンク未満に抑えるため。
    static constexpr size_t kDedicatedThreshold = kMaxChunkSize / 4;

    void* Allocate(size_t size, size_t align)
    {
        void* p = cur_;
        size_t space = static_cast<size_t>(end_ - cur_);
        if (std::align(align, size, p, space)) {
            cur_ = static_cast<std::byte*>(p) + size;
            return p;
        }
        return AllocateSlow(size, align);
    }

    void* AllocateSlow(size_t size, [[maybe_unused]] size_t align)
    {
        // new[] の先頭は __STDCPP_DEFAULT_NEW_ALIGNMENT__ に揃う。
        assert(align <= __STDCPP_DEFAULT_NEW_ALIGNMENT__);
        if (size > kDedicatedThreshold) {
            return chunks_.emplace_back(std::make_unique_for_overwrite<std::byte[]>(size)).get();
        }
        const size_t chunk_size = std::max(next_chunk_size_, size);
        next_chunk_size_ = std::min(next_chunk_size_ * 2, kMaxChunkSize);
        std::byte* const chunk = chunks_.emplace_back(std::make_unique_for_overwrite<std::byte[]>(chunk_size)).get();
        cur_ = chunk + size;
        end_ = chunk + chunk_size;
        return chunk;
    }

    std::vector<std::unique_ptr<std::byte[]>> chunks_;
    std::vector<ReservedArray<char>> adopted_;
    std::byte* cur_ = nullptr;
    std::byte* end_ = nullptr;
    size_t next_chunk_size_ = kMinChunkSize;
};
