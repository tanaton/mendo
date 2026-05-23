#pragma once
#include <cstdint>
#include <functional>
#include <memory_resource>
#include <string>
#include <string_view>

namespace mendo {

// UTF-8 byte 単位。
using doc_offset = uint32_t;

inline constexpr char doc_lf = '\n';
inline constexpr char doc_cr = '\r';
inline constexpr char doc_tab = '\t';
inline constexpr char doc_sp = ' ';

struct StringTransparentHash {
    using is_transparent = void;
    size_t operator()(std::string_view sv) const noexcept
    {
        return std::hash<std::string_view>{}(sv);
    }
    size_t operator()(const std::pmr::string& s) const noexcept
    {
        return std::hash<std::string_view>{}(s);
    }
};

} // namespace mendo

