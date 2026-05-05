#pragma once
#include <cstdint>
#include <functional>
#include <memory_resource>
#include <string>
#include <string_view>

// Document の 1 次的なテキスト表現の文字型を表す alias 群。
// MENDO_DOC_USE_UTF16=1 (現状) では wchar_t/UTF-16、=0 では char/UTF-8 にビルド時切替する。
// Win32/DirectWrite 用の「描画用 wchar_t」とは概念上分離 (後者は alias 影響外で wchar_t 維持)。
//
// 切替点は CMakeLists.txt の MENDO_DOC_USE_UTF16 オプション 1 箇所に集約。
// MD4C_USE_UTF16 (md4c の MD_CHAR=WCHAR 制御) と連動させる。

namespace mendo {

#ifndef MENDO_DOC_USE_UTF16
#  define MENDO_DOC_USE_UTF16 1
#endif

#if MENDO_DOC_USE_UTF16
using doc_char        = wchar_t;
using doc_string      = std::pmr::wstring;
using doc_string_std  = std::wstring;
using doc_string_view = std::wstring_view;
#  define MENDO_LIT(s) L##s
#  define MENDO_LITR(s) LR##s
#else
using doc_char        = char;
using doc_string      = std::pmr::string;
using doc_string_std  = std::string;
using doc_string_view = std::string_view;
#  define MENDO_LIT(s) s
#  define MENDO_LITR(s) R##s
#endif

// doc_char 単位 offset。意味は切替前後で変わる (UTF-16 code unit ↔ UTF-8 byte) ので、
// 単位を意識すべき箇所のマーカーとして専用 alias にしておく。
using doc_offset = uint32_t;

inline constexpr doc_char doc_lf  = MENDO_LIT('\n');
inline constexpr doc_char doc_cr  = MENDO_LIT('\r');
inline constexpr doc_char doc_tab = MENDO_LIT('\t');
inline constexpr doc_char doc_sp  = MENDO_LIT(' ');

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

// UTF-16 ビルド時は型同一なので、doc_string ↔ wstring 互換変換は string_convert.h の
// WideToUtf8 / Utf8ToWide を経由する (UTF-8 ビルド時のみ実体変換が走る)。
// これらヘルパは Win32 API 互換境界 (CF_UNICODETEXT クリップボード, ShellExecuteW 等) で使う。

