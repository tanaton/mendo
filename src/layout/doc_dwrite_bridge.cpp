#include "doc_dwrite_bridge.h"
#include "memory_resource.h"
#include "utf8_codec.h"
#include <algorithm>
#include <emmintrin.h>
#include <limits>
#include <numeric>

namespace mendo {

namespace {

// offsets が非 null なら対応表も作る。
// offsets[i] (i = 0..utf8_size) = doc byte i 直前までの累積 wide unit 数。
// - leading byte 位置: その文字を出力する直前の wide_pos
// - continuation byte 位置: 同じ wide_pos (sequence 中はマップ値据え置き)
// - 末尾 (= utf8_size): 全 wide unit 数 (番兵)
// これにより `[byte_a, byte_b)` の範囲は wide で `[map[a], map[b])` に変換できる。
// MultiByteToWideChar は不正バイトの扱いが OS 依存なので、Utf16OffsetCursor と同じ規則の自前 decode に揃える。
void DecodeUtf8(std::string_view text, std::pmr::wstring& out, std::pmr::vector<uint32_t>* offsets)
{
    if (text.empty() || text.size() > static_cast<size_t>(std::numeric_limits<int>::max())) {
        return;
    }
    const size_t n = text.size();
    out.reserve(n); // UTF-16 code unit 数 <= UTF-8 byte 数

    // 先頭の ASCII 区間は恒等写像なので、全体が ASCII なら対応表を確保しない。
    size_t byte_pos = 0;
    while (byte_pos < n && static_cast<unsigned char>(text[byte_pos]) < 0x80) {
        out.push_back(static_cast<wchar_t>(text[byte_pos]));
        ++byte_pos;
    }
    if (byte_pos == n) {
        return;
    }
    if (offsets) {
        offsets->resize(n + 1); // 番兵込み
        std::iota(offsets->begin(), offsets->begin() + static_cast<std::ptrdiff_t>(byte_pos), 0u);
    }

    uint32_t wide_pos = static_cast<uint32_t>(byte_pos);
    while (byte_pos < n) {
        const auto decoded = utf8_codec::DecodeAt(text, static_cast<uint32_t>(byte_pos));
        if (offsets) {
            std::fill_n(offsets->begin() + static_cast<std::ptrdiff_t>(byte_pos), decoded.len, wide_pos);
        }
        if (decoded.cp <= 0xFFFF) {
            out.push_back(static_cast<wchar_t>(decoded.cp));
            ++wide_pos;
        }
        else {
            const uint32_t adj = decoded.cp - 0x10000u;
            out.push_back(static_cast<wchar_t>(0xD800u + (adj >> 10)));
            out.push_back(static_cast<wchar_t>(0xDC00u + (adj & 0x3FFu)));
            wide_pos += 2;
        }
        byte_pos += decoded.len;
    }
    if (offsets) {
        (*offsets)[n] = wide_pos;
    }
}

} // namespace

WideViewForDWrite::WideViewForDWrite(std::string_view text)
    : scratch_(GetThreadLocalPoolResource()),
      utf16_offsets_(GetThreadLocalPoolResource())
{
    DecodeUtf8(text, scratch_, &utf16_offsets_);
}

std::pmr::wstring DecodeForDWrite(std::string_view text)
{
    std::pmr::wstring out(GetThreadLocalPoolResource());
    DecodeUtf8(text, out, nullptr);
    return out;
}

uint32_t Utf16OffsetCursor::WideAt(uint32_t byte_target) noexcept
{
    byte_target = std::min(byte_target, static_cast<uint32_t>(text_.size()));
    while (byte_ < byte_target) {
        // ASCII 区間は byte と UTF-16 が 1:1 なので 16 byte 単位で読み飛ばす。
        if (byte_ + 16 <= byte_target) {
            const __m128i c = _mm_loadu_si128(reinterpret_cast<const __m128i*>(text_.data() + byte_));
            if (_mm_movemask_epi8(c) == 0) {
                byte_ += 16;
                wide_ += 16;
                continue;
            }
        }
        const auto decoded = utf8_codec::DecodeAt(text_, byte_);
        if (byte_ + decoded.len > byte_target) {
            break;
        }
        wide_ += (decoded.cp > 0xFFFF) ? 2u : 1u;
        byte_ += decoded.len;
    }
    return wide_;
}

uint32_t WideViewForDWrite::WideOffsetFromDocOffset(doc_offset doc_off) const noexcept
{
    if (utf16_offsets_.empty()) {
        return std::min(doc_off, static_cast<uint32_t>(scratch_.size()));
    }
    if (doc_off >= utf16_offsets_.size()) {
        return utf16_offsets_.back();
    }
    return utf16_offsets_[doc_off];
}

doc_offset WideViewForDWrite::DocOffsetFromWideOffset(uint32_t wide_off) const noexcept
{
    if (utf16_offsets_.empty()) {
        return std::min(wide_off, static_cast<uint32_t>(scratch_.size()));
    }
    if (wide_off >= utf16_offsets_.back()) {
        return static_cast<doc_offset>(utf16_offsets_.size() - 1);
    }
    // utf16_offsets_ は弱増加 (continuation byte は同じ wide_pos)。lower_bound で
    // wide_off に対応する文字の leading byte 位置を取る。upper_bound 相当だと末尾文字の
    // continuation byte を返してしまい、抽出末尾が UTF-8 不正で文字化けする。
    const auto it = std::ranges::lower_bound(utf16_offsets_, wide_off);
    return static_cast<doc_offset>(it - utf16_offsets_.begin());
}

HRESULT CreateDocTextLayout(
    IDWriteFactory* factory, std::wstring_view wide,
    IDWriteTextFormat* fmt, float max_w, float max_h,
    IDWriteTextLayout** out) noexcept
{
    return factory->CreateTextLayout(wide.data(), static_cast<UINT32>(wide.size()), fmt, max_w, max_h, out);
}

} // namespace mendo
