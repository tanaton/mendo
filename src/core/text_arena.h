#pragma once
#include <cstddef>
#include <cstring>
#include <memory_resource>
#include <ranges>
#include <span>
#include <string_view>
#include <type_traits>

// パース後に書き換えないノードのテキスト・TextRun・リンク URL を置く領域。
// 個別には解放せず、Document の差し替え・破棄のときにまとめて捨てる。
class TextArena {
public:
    TextArena() = default;
    explicit TextArena(size_t initial_size) : resource_(initial_size) {}

    std::string_view CopyText(std::string_view text)
    {
        if (text.empty()) {
            return {};
        }
        auto* const p = static_cast<char*>(resource_.allocate(text.size(), alignof(char)));
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
        void* const p = resource_.allocate(src.size_bytes(), alignof(T));
        std::memcpy(p, src.data(), src.size_bytes());
        return { static_cast<const T*>(p), src.size() };
    }

private:
    std::pmr::monotonic_buffer_resource resource_;
};
