#include "doc_dwrite_bridge.h"
#include "utf8_codec.h"
#include <algorithm>
#include <emmintrin.h>
#include <limits>

// MultiByteToWideChar は不正バイトの扱いが OS 依存なので、Utf16OffsetCursor と同じ規則の自前 decode に揃える。
void DecodeForDWrite(std::string_view text, std::pmr::wstring& out)
{
    out.clear();
    if (text.empty() || text.size() > static_cast<size_t>(std::numeric_limits<int>::max())) {
        return;
    }
    const size_t n = text.size();
    out.reserve(n); // UTF-16 code unit 数 <= UTF-8 byte 数

    size_t byte_pos = 0;
    while (byte_pos < n) {
        if (static_cast<unsigned char>(text[byte_pos]) < 0x80) {
            out.push_back(static_cast<wchar_t>(text[byte_pos]));
            ++byte_pos;
            continue;
        }
        const auto decoded = utf8_codec::DecodeAt(text, static_cast<uint32_t>(byte_pos));
        if (decoded.cp <= 0xFFFF) {
            out.push_back(static_cast<wchar_t>(decoded.cp));
        }
        else {
            const uint32_t adj = decoded.cp - 0x10000u;
            out.push_back(static_cast<wchar_t>(0xD800u + (adj >> 10)));
            out.push_back(static_cast<wchar_t>(0xDC00u + (adj & 0x3FFu)));
        }
        byte_pos += decoded.len;
    }
}

bool Utf16OffsetCursor::SkipAscii16() noexcept
{
    const __m128i c = _mm_loadu_si128(reinterpret_cast<const __m128i*>(text_.data() + byte_));
    if (_mm_movemask_epi8(c) != 0) {
        return false;
    }
    byte_ += 16;
    wide_ += 16;
    return true;
}

void Utf16OffsetCursor::Advance(const utf8_codec::DecodedCp& decoded) noexcept
{
    wide_ += (decoded.cp > 0xFFFF) ? 2u : 1u;
    byte_ += decoded.len;
}

uint32_t Utf16OffsetCursor::WideAt(uint32_t byte_target) noexcept
{
    byte_target = std::min(byte_target, static_cast<uint32_t>(text_.size()));
    while (byte_ < byte_target) {
        if (byte_ + 16 <= byte_target && SkipAscii16()) {
            continue;
        }
        const auto decoded = utf8_codec::DecodeAt(text_, byte_);
        if (byte_ + decoded.len > byte_target) {
            break;
        }
        Advance(decoded);
    }
    return wide_;
}

uint32_t Utf16OffsetCursor::ByteAt(uint32_t wide_target) noexcept
{
    const auto size = static_cast<uint32_t>(text_.size());
    while (byte_ < size && wide_ < wide_target) {
        if (byte_ + 16 <= size && wide_ + 16 <= wide_target && SkipAscii16()) {
            continue;
        }
        Advance(utf8_codec::DecodeAt(text_, byte_));
    }
    return byte_;
}

// CreateTextLayout は文字列をコピーするので、変換先はスレッドごとに使い回してセル・ノード単位の確保を避ける。
// 長文で膨らんだ容量は手放す (計測ワーカーごとに常駐させない)。
HRESULT CreateDocTextLayout(
    IDWriteFactory* factory, std::string_view text,
    IDWriteTextFormat* fmt, float max_w, float max_h,
    IDWriteTextLayout** out)
{
    constexpr size_t kMaxRetainedChars = 8 * 1024;
    thread_local std::pmr::wstring wide;
    DecodeForDWrite(text, wide);
    const HRESULT hr = factory->CreateTextLayout(wide.data(), static_cast<UINT32>(wide.size()), fmt, max_w, max_h, out);
    if (wide.capacity() > kMaxRetainedChars) {
        std::pmr::wstring().swap(wide);
    }
    return hr;
}
