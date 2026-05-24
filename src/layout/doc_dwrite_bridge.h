#pragma once
#include "doc_text.h"
#include <cstddef>
#include <cstdint>
#include <dwrite.h>
#include <memory_resource>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// Document テキスト (UTF-8 string_view) を IDWriteTextLayout / IDWriteTextFormat に
// 渡すための境界ラッパー。MultiByteToWideChar 相当の自前 decode で per-call wide スクラッチを
// 構築し、HitTest/Search 用に UTF-8 byte ↔ UTF-16 code unit 対応表 (utf16_offsets_) を保持する。
//
// 描画用 wchar_t (フォント名 / locale / i18n リテラル等) はこの境界の対象外。
// それらは std::wstring 経路を維持し、CreateDocTextLayout / WideViewForDWrite を経由しない。

namespace mendo {

class WideViewForDWrite {
public:
    explicit WideViewForDWrite(std::string_view text);

    std::wstring_view wide() const noexcept
    {
        return view_;
    }

    // 範囲外時は clamping。
    uint32_t WideOffsetFromDocOffset(doc_offset doc_off) const noexcept;

    // 範囲外時は clamping。
    doc_offset DocOffsetFromWideOffset(uint32_t wide_off) const noexcept;

    DWRITE_TEXT_RANGE WideRange(uint32_t doc_start, uint32_t doc_length) const noexcept
    {
        const uint32_t w_start = WideOffsetFromDocOffset(doc_start);
        const uint32_t w_end = WideOffsetFromDocOffset(doc_start + doc_length);
        return DWRITE_TEXT_RANGE{ w_start, w_end - w_start };
    }

private:
    // view_ の所有元。
    std::pmr::wstring scratch_;
    std::wstring_view view_;
    // 番兵: [utf8_size] = wide_size。
    std::pmr::vector<uint32_t> utf16_offsets_;
};

// 直近に渡された文字列に対する WideViewForDWrite を再利用する identity ベースキャッシュ。
// std::string_view の (data ポインタ + size) で同一性を判定し、異なる文字列が来た時のみ
// 再構築する。連続フレームで同じノード/セルの選択や検索ハイライトを描画する典型ケースで
// UTF-8→UTF-16 decode を 1 回に抑える。
//
// ドキュメント差し替え等で内部 string_view の data ポインタが dangling 化しうる場合は
// Reset() か ResetIfBufferChanged() で明示的に無効化すること (PMR pool が解放後アドレスを
// 再利用するため、ノード配列等の (data ポインタ + size) を別途追跡する用途で
// ResetIfBufferChanged() を提供している)。
class WideViewCache {
public:
    const WideViewForDWrite& Get(std::string_view text)
    {
        if (text.data() != text_.data() || text.size() != text_.size()) {
            wv_.emplace(text);
            text_ = text;
        }
        return *wv_;
    }

    DWRITE_TEXT_RANGE WideRange(std::string_view text, uint32_t doc_start, uint32_t doc_length)
    {
        return Get(text).WideRange(doc_start, doc_length);
    }

    void Reset() noexcept
    {
        wv_.reset();
        text_ = {};
    }

    // 任意の連続バッファ (典型的にはノード配列) の identity が前回呼び出しから
    // 変わっていれば Reset() し、識別子を更新する。同一なら何もしない。
    template <typename T>
    void ResetIfBufferChanged(const T* data, size_t size) noexcept
    {
        const auto* p = static_cast<const void*>(data);
        if (p == owner_data_ && size == owner_size_) {
            return;
        }
        Reset();
        owner_data_ = p;
        owner_size_ = size;
    }

private:
    std::string_view text_;
    std::optional<WideViewForDWrite> wv_;
    const void* owner_data_ = nullptr;
    size_t owner_size_ = 0;
};

// ApplyRunFormatting と view を共有し二重 decode を回避。
HRESULT CreateDocTextLayout(
    IDWriteFactory* factory, const WideViewForDWrite& view,
    IDWriteTextFormat* fmt, float max_w, float max_h,
    IDWriteTextLayout** out) noexcept;

HRESULT CreateDocTextLayout(
    IDWriteFactory* factory, std::string_view text,
    IDWriteTextFormat* fmt, float max_w, float max_h,
    IDWriteTextLayout** out) noexcept;

} // namespace mendo
