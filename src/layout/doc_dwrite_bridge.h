#pragma once
#include "doc_text.h"
#include <cstdint>
#include <dwrite.h>
#include <memory_resource>
#include <string>
#include <string_view>
#include <vector>

// Document テキスト (UTF-8 doc_string_view) を IDWriteTextLayout / IDWriteTextFormat に
// 渡すための境界ラッパー。MultiByteToWideChar 相当の自前 decode で per-call wide スクラッチを
// 構築し、HitTest/Search 用に UTF-8 byte ↔ UTF-16 code unit 対応表 (utf16_offsets_) を保持する。
//
// 描画用 wchar_t (フォント名 / locale / i18n リテラル等) はこの境界の対象外。
// それらは std::wstring 経路を維持し、CreateDocTextLayout / WideViewForDWrite を経由しない。

namespace mendo {

class WideViewForDWrite {
public:
    explicit WideViewForDWrite(doc_string_view text);

    // IDWriteTextLayout / IDWriteTextFormat に渡せる UTF-16 ビュー。
    std::wstring_view wide() const noexcept
    {
        return view_;
    }

    // doc_offset (UTF-8 byte offset) → IDWriteTextLayout 内 UTF-16 textPosition。
    // 範囲外時は wide().size() を返す (clamping)。
    uint32_t WideOffsetFromDocOffset(doc_offset doc_off) const noexcept;

    // IDWriteTextLayout::HitTestPoint 等で得られた textPosition (UTF-16 code unit) を
    // doc_offset (UTF-8 byte) に還元する。範囲外時は doc 文字列長を返す (clamping)。
    doc_offset DocOffsetFromWideOffset(uint32_t wide_off) const noexcept;

    // doc_offset 単位の [start, start+length) 範囲を IDWriteTextLayout 用 (UTF-16) に変換。
    DWRITE_TEXT_RANGE WideRange(uint32_t doc_start, uint32_t doc_length) const noexcept
    {
        const uint32_t w_start = WideOffsetFromDocOffset(doc_start);
        const uint32_t w_end = WideOffsetFromDocOffset(doc_start + doc_length);
        return DWRITE_TEXT_RANGE{ w_start, w_end - w_start };
    }

private:
    // scratch_ が view_ の所有元。コンストラクタで MultiByteToWideChar 相当の 1-pass decode で構築。
    std::pmr::wstring scratch_;
    std::wstring_view view_;
    // utf16_offsets_[i] (i = 0..utf8_size) = doc 内 byte i に対応する wide offset。
    // 番兵: utf16_offsets_[utf8_size] = wide_size。
    std::pmr::vector<uint32_t> utf16_offsets_;
};

// IDWriteFactory::CreateTextLayout の境界ラッパー。
// 構築済み view を渡すオーバーロードは ApplyRunFormatting と同じ view を共有するために使う
// (per-node の二重 UTF-8→UTF-16 decode を回避)。
HRESULT CreateDocTextLayout(IDWriteFactory* factory, const WideViewForDWrite& view,
                            IDWriteTextFormat* fmt, float max_w, float max_h,
                            IDWriteTextLayout** out) noexcept;

// 互換オーバーロード: text のみ渡したい呼び出し側向け (内部で WideViewForDWrite を構築)。
HRESULT CreateDocTextLayout(IDWriteFactory* factory, doc_string_view text,
                            IDWriteTextFormat* fmt, float max_w, float max_h,
                            IDWriteTextLayout** out) noexcept;

} // namespace mendo
