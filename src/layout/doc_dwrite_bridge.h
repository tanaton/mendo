#pragma once
#include "doc_text.h"
#include <cstdint>
#include <dwrite.h>
#include <memory_resource>
#include <string>
#include <string_view>
#include <vector>

// Document テキスト (doc_string_view) を IDWriteTextLayout / IDWriteTextFormat に
// 渡すための境界ラッパー。
//
// MENDO_DOC_USE_UTF16=1 (現状): doc_string_view は std::wstring_view と等価で zero-copy。
//   WideViewForDWrite は wstring_view を保持するだけで、offset 変換も恒等関数。
// MENDO_DOC_USE_UTF16=0 (切替後): doc_string_view は std::string_view (UTF-8)。
//   WideViewForDWrite は MultiByteToWideChar で per-call wide スクラッチを構築し、
//   HitTest/Search 用に UTF-8 byte ↔ UTF-16 code unit 対応表を保持する。
//
// 描画用 wchar_t (フォント名 / locale / i18n リテラル等) はこの境界の対象外。
// それらは std::wstring 経路を維持し、CreateDocTextLayout/WideViewForDWrite を経由しない。

namespace mendo {

class WideViewForDWrite {
public:
    explicit WideViewForDWrite(doc_string_view text);

    // IDWriteTextLayout / IDWriteTextFormat に渡せる UTF-16 ビュー。
    std::wstring_view wide() const noexcept
    {
        return view_;
    }

    // doc_offset (= doc_char 単位 offset) → IDWriteTextLayout 内 UTF-16 textPosition。
    // UTF-16 ビルドでは恒等関数。UTF-8 ビルドでは byte → code unit のルックアップ。
    // 範囲外時は wide().size() を返す (clamping)。
    uint32_t WideOffsetFromDocOffset(doc_offset doc_off) const noexcept;

    // IDWriteTextLayout::HitTestPoint 等で得られた textPosition (UTF-16 code unit) を
    // doc_offset に還元する。範囲外時は doc 文字列長を返す (clamping)。
    doc_offset DocOffsetFromWideOffset(uint32_t wide_off) const noexcept;

    // doc_offset 単位の [start, start+length) 範囲を IDWriteTextLayout 用 (UTF-16) に変換。
    DWRITE_TEXT_RANGE WideRange(uint32_t doc_start, uint32_t doc_length) const noexcept
    {
        const uint32_t w_start = WideOffsetFromDocOffset(doc_start);
        const uint32_t w_end = WideOffsetFromDocOffset(doc_start + doc_length);
        return DWRITE_TEXT_RANGE{ w_start, w_end - w_start };
    }

private:
#if MENDO_DOC_USE_UTF16
    std::wstring_view view_;
#else
    // scratch_ が view_ の所有元。MultiByteToWideChar で 1 回構築。
    std::pmr::wstring scratch_;
    std::wstring_view view_;
    // utf16_offsets_[i] (i = 0..utf8_size) = doc 内 byte i に対応する wide offset。
    // 番兵: utf16_offsets_[utf8_size] = wide_size。HitTest 用途以外では空のまま。
    // Measure ホットパスでは構築せず、offset 変換要求時に lazy 構築する想定。
    std::pmr::vector<uint32_t> utf16_offsets_;
    // utf16_offsets_ が構築済みかどうか (空スクラッチ時との区別)。
    mutable bool offsets_built_ = false;
    void EnsureOffsetMap() const noexcept;
#endif
};

// IDWriteFactory::CreateTextLayout の境界ラッパー。
// UTF-16 ビルドでは zero-copy で wide ポインタを直接渡す。
// UTF-8 ビルドでは内部で WideViewForDWrite を構築して per-call 変換する。
HRESULT CreateDocTextLayout(IDWriteFactory* factory, doc_string_view text,
                            IDWriteTextFormat* fmt, float max_w, float max_h,
                            IDWriteTextLayout** out) noexcept;

#if MENDO_DOC_USE_UTF16
inline WideViewForDWrite::WideViewForDWrite(doc_string_view text)
    : view_(text)
{
}

inline uint32_t WideViewForDWrite::WideOffsetFromDocOffset(doc_offset doc_off) const noexcept
{
    const uint32_t n = static_cast<uint32_t>(view_.size());
    return doc_off > n ? n : doc_off;
}

inline doc_offset WideViewForDWrite::DocOffsetFromWideOffset(uint32_t wide_off) const noexcept
{
    const uint32_t n = static_cast<uint32_t>(view_.size());
    return wide_off > n ? n : wide_off;
}

inline HRESULT CreateDocTextLayout(IDWriteFactory* factory, doc_string_view text,
                                   IDWriteTextFormat* fmt, float max_w, float max_h,
                                   IDWriteTextLayout** out) noexcept
{
    return factory->CreateTextLayout(text.data(), static_cast<UINT32>(text.size()),
                                     fmt, max_w, max_h, out);
}
#endif

} // namespace mendo
