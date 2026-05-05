#include "doc_dwrite_bridge.h"
#include "memory_resource.h"
#include <limits>

namespace mendo {

WideViewForDWrite::WideViewForDWrite(doc_string_view text)
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
    // UTF-8 → UTF-16 変換を自前で行い、同時に byte index → wide unit offset の対応表を埋める。
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

    const auto* const begin = reinterpret_cast<const unsigned char*>(text.data());
    const auto* const end = begin + text.size();
    auto* it = begin;
    uint32_t wide_pos = 0;
    while (it != end) {
        const size_t byte_pos = static_cast<size_t>(it - begin);
        utf16_offsets_[byte_pos] = wide_pos;
        const unsigned char b0 = *it;
        if (b0 < 0x80) {
            scratch_.push_back(static_cast<wchar_t>(b0));
            ++wide_pos;
            ++it;
        }
        else if ((b0 & 0xE0) == 0xC0 && it + 1 < end) {
            const uint32_t cp = ((b0 & 0x1Fu) << 6) | (it[1] & 0x3Fu);
            scratch_.push_back(static_cast<wchar_t>(cp));
            utf16_offsets_[byte_pos + 1] = wide_pos;
            ++wide_pos;
            it += 2;
        }
        else if ((b0 & 0xF0) == 0xE0 && it + 2 < end) {
            const uint32_t cp = ((b0 & 0x0Fu) << 12) | ((it[1] & 0x3Fu) << 6) | (it[2] & 0x3Fu);
            scratch_.push_back(static_cast<wchar_t>(cp));
            utf16_offsets_[byte_pos + 1] = wide_pos;
            utf16_offsets_[byte_pos + 2] = wide_pos;
            ++wide_pos;
            it += 3;
        }
        else if ((b0 & 0xF8) == 0xF0 && it + 3 < end) {
            const uint32_t cp = ((b0 & 0x07u) << 18) | ((it[1] & 0x3Fu) << 12) | ((it[2] & 0x3Fu) << 6) | (it[3] & 0x3Fu);
            // 補助面: UTF-16 サロゲートペア (2 wide units)
            const uint32_t adj = cp - 0x10000u;
            scratch_.push_back(static_cast<wchar_t>(0xD800u + (adj >> 10)));
            scratch_.push_back(static_cast<wchar_t>(0xDC00u + (adj & 0x3FFu)));
            utf16_offsets_[byte_pos + 1] = wide_pos;
            utf16_offsets_[byte_pos + 2] = wide_pos;
            utf16_offsets_[byte_pos + 3] = wide_pos;
            wide_pos += 2;
            it += 4;
        }
        else {
            // 不正先頭バイト or truncated。プレースホルダ U+FFFD 1 wide unit。
            scratch_.push_back(L'\xFFFD');
            ++wide_pos;
            ++it;
        }
    }
    utf16_offsets_[text.size()] = wide_pos;
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
    // 単調増加なので二分探索。
    size_t lo = 0;
    size_t hi = utf16_offsets_.size();
    while (lo + 1 < hi) {
        const size_t mid = lo + (hi - lo) / 2;
        if (utf16_offsets_[mid] <= wide_off) {
            lo = mid;
        }
        else {
            hi = mid;
        }
    }
    return static_cast<doc_offset>(lo);
}

HRESULT CreateDocTextLayout(IDWriteFactory* factory, doc_string_view text,
                            IDWriteTextFormat* fmt, float max_w, float max_h,
                            IDWriteTextLayout** out) noexcept
{
    WideViewForDWrite view{text};
    const auto wide = view.wide();
    return factory->CreateTextLayout(wide.data(), static_cast<UINT32>(wide.size()),
                                     fmt, max_w, max_h, out);
}

} // namespace mendo
