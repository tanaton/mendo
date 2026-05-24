#include "doc_dwrite_bridge.h"
#include "memory_resource.h"
#include "utf8_codec.h"
#include <algorithm>
#include <limits>

namespace mendo {

WideViewForDWrite::WideViewForDWrite(std::string_view text)
    : scratch_(GetThreadLocalPoolResource()),
      utf16_offsets_(GetThreadLocalPoolResource())
{
    if (text.empty()) {
        view_ = {};
        return;
    }
    if (text.size() > static_cast<size_t>(std::numeric_limits<int>::max())) {
        view_ = {};
        return;
    }
    // utf16_offsets_[i] (i = 0..utf8_size) = doc byte i 直前までの累積 wide unit 数。
    // - leading byte 位置: その文字を出力する直前の wide_pos
    // - continuation byte 位置: 同じ wide_pos (sequence 中はマップ値据え置き)
    // - 末尾 (= utf8_size): 全 wide unit 数 (番兵)
    // これにより `[byte_a, byte_b)` の範囲は wide で `[map[a], map[b])` に変換できる。
    //
    // MultiByteToWideChar より自前 decode が遅いが、MeasureNode の per-node コストとしては許容範囲。
    // MultiByteToWideChar で先に wide を作って 2-pass にする選択肢もあるが、整合性 (decode ルールが
    // OS 依存) を避けて 1-pass で統一。
    scratch_.reserve(text.size());          // UTF-16 code unit 数 <= UTF-8 byte 数
    utf16_offsets_.resize(text.size() + 1); // 番兵込み

    const size_t n = text.size();
    uint32_t wide_pos = 0;
    size_t byte_pos = 0;
    while (byte_pos < n) {
        const auto decoded = utf8_codec::DecodeAt(text, static_cast<uint32_t>(byte_pos));
        for (uint32_t i = 0; i < decoded.len; ++i) {
            utf16_offsets_[byte_pos + i] = wide_pos;
        }
        if (decoded.cp <= 0xFFFF) {
            scratch_.push_back(static_cast<wchar_t>(decoded.cp));
            ++wide_pos;
        }
        else {
            const uint32_t adj = decoded.cp - 0x10000u;
            scratch_.push_back(static_cast<wchar_t>(0xD800u + (adj >> 10)));
            scratch_.push_back(static_cast<wchar_t>(0xDC00u + (adj & 0x3FFu)));
            wide_pos += 2;
        }
        byte_pos += decoded.len;
    }
    utf16_offsets_[n] = wide_pos;
    view_ = scratch_;
}

uint32_t WideViewForDWrite::WideOffsetFromDocOffset(doc_offset doc_off) const noexcept
{
    if (utf16_offsets_.empty()) {
        return 0;
    }
    if (doc_off >= utf16_offsets_.size()) {
        return utf16_offsets_.back();
    }
    return utf16_offsets_[doc_off];
}

doc_offset WideViewForDWrite::DocOffsetFromWideOffset(uint32_t wide_off) const noexcept
{
    if (utf16_offsets_.empty()) {
        return 0;
    }
    if (wide_off >= utf16_offsets_.back()) {
        return static_cast<doc_offset>(utf16_offsets_.size() - 1);
    }
    // utf16_offsets_ は弱増加 (continuation byte は同じ wide_pos)。lower_bound で
    // wide_off に対応する文字の leading byte 位置を取る。upper_bound 相当だと末尾文字の
    // continuation byte を返してしまい、抽出末尾が UTF-8 不正で文字化けする。
    const auto it = std::lower_bound(utf16_offsets_.begin(), utf16_offsets_.end(), wide_off);
    return static_cast<doc_offset>(it - utf16_offsets_.begin());
}

HRESULT CreateDocTextLayout(
    IDWriteFactory* factory, const WideViewForDWrite& view,
    IDWriteTextFormat* fmt, float max_w, float max_h,
    IDWriteTextLayout** out) noexcept
{
    const auto wide = view.wide();
    return factory->CreateTextLayout(wide.data(), static_cast<UINT32>(wide.size()), fmt, max_w, max_h, out);
}

HRESULT CreateDocTextLayout(
    IDWriteFactory* factory, std::string_view text,
    IDWriteTextFormat* fmt, float max_w, float max_h,
    IDWriteTextLayout** out) noexcept
{
    WideViewForDWrite view{ text };
    return CreateDocTextLayout(factory, view, fmt, max_w, max_h, out);
}

} // namespace mendo
