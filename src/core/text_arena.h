#pragma once
#include <algorithm>
#include <cassert>
#include <cstddef>
#include <cstring>
#include <memory>
#include <ranges>
#include <span>
#include <string_view>
#include <type_traits>
#include <vector>

// パース後に書き換えないノードのテキスト・TextRun・リンク URL を置く領域。
// 個別には解放せず、Document の差し替え・破棄のときにまとめて捨てる。
// チャンクを積み足すだけのバンプアロケータ。チャンクは小さく始めて kMaxChunkSize まで倍々に広げ、
// 小さな文書で大きく確保せず、大きな文書でも使わずに残る領域を末尾のチャンク 1 個分程度に抑える。
class TextArena {
public:
    std::string_view CopyText(std::string_view text)
    {
        if (text.empty()) {
            return {};
        }
        auto* const p = static_cast<char*>(Allocate(text.size(), alignof(char)));
        std::memcpy(p, text.data(), text.size());
        return { p, text.size() };
    }

    // TextRun や、raw_text_ / arena 上の文字列を指す string_view の配列を置く。
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
    std::byte* cur_ = nullptr;
    std::byte* end_ = nullptr;
    size_t next_chunk_size_ = kMinChunkSize;
};
