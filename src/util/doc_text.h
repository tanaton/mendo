#pragma once
#include <cstdint>
#include <functional>
#include <memory_resource>
#include <string>
#include <string_view>

// Document の 1 次的なテキスト表現の文字型を表す alias 群。
// 実体は UTF-8 (char ベース) で固定。Win32/DirectWrite 用の「描画用 wchar_t」とは
// 概念上分離 (後者は alias 影響外で wchar_t 維持)。
//
// MENDO_LIT(s) はかつて UTF-16/UTF-8 切替に使用していた残骸で、現在は no-op だが
// 「Document テキスト用リテラル」のセマンティクスマーカーとして残置する。

namespace mendo {

using doc_char        = char;
using doc_string      = std::pmr::string;
using doc_string_std  = std::string;
using doc_string_view = std::string_view;
#define MENDO_LIT(s) s
#define MENDO_LITR(s) R##s

// doc_char 単位 offset (UTF-8 byte)。単位を意識すべき箇所のマーカーとして専用 alias。
using doc_offset = uint32_t;

inline constexpr doc_char doc_lf  = MENDO_LIT('\n');
inline constexpr doc_char doc_cr  = MENDO_LIT('\r');
inline constexpr doc_char doc_tab = MENDO_LIT('\t');
inline constexpr doc_char doc_sp  = MENDO_LIT(' ');

template <typename T>
inline doc_string_std to_doc_string(T value)
{
    return std::to_string(value);
}

// doc_string と doc_string_view を等価にハッシュする透過ハッシャ。
// equal_to<> と組み合わせて unordered_map に渡すと doc_string_view からの lookup で
// 一時 doc_string の確保をスキップできる。
struct DocStringTransparentHash {
    using is_transparent = void;
    size_t operator()(doc_string_view sv) const noexcept
    {
        return std::hash<doc_string_view>{}(sv);
    }
    size_t operator()(const doc_string& s) const noexcept
    {
        return std::hash<doc_string_view>{}(s);
    }
};

} // namespace mendo

// doc_string ↔ wstring 互換変換は string_convert.h の Utf8ToWide / WideToUtf8 を経由する。
// Win32 API 互換境界 (CF_UNICODETEXT クリップボード, ShellExecuteW 等) で使う。

