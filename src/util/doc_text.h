#pragma once
#include <cstdint>
#include <functional>
#include <memory_resource>
#include <string>
#include <string_view>

// Document の 1 次的なテキスト表現の文字型を表す alias 群。
// 実体は UTF-8 (char ベース) で固定。

namespace mendo {

// char 単位 offset (UTF-8 byte)。単位を意識すべき箇所のマーカーとして専用 alias。
using doc_offset = uint32_t;

inline constexpr char doc_lf = '\n';
inline constexpr char doc_cr = '\r';
inline constexpr char doc_tab = '\t';
inline constexpr char doc_sp = ' ';

// string と string_view を等価にハッシュする透過ハッシャ。
// equal_to<> と組み合わせて unordered_map に渡すと string_view からの lookup で
// 一時 string の確保をスキップできる。
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

// string ↔ wstring 互換変換は string_convert.h の Utf8ToWide / WideToUtf8 を経由する。
// Win32 API 互換境界 (CF_UNICODETEXT クリップボード, ShellExecuteW 等) で使う。
