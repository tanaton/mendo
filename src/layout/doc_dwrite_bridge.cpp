#include "doc_dwrite_bridge.h"
#include "utf8_codec.h"
#include <algorithm>
#include <bit>
#include <emmintrin.h>
#include <limits>

// MultiByteToWideChar は不正バイトの扱いが OS 依存なので、Utf16OffsetCursor と同じ規則の自前 decode に揃える。
void DecodeForDWrite(std::string_view text, std::wstring& out)
{
    out.clear();
    if (text.empty() || text.size() > static_cast<size_t>(std::numeric_limits<int>::max())) {
        return;
    }
    const size_t n = text.size();
    // UTF-16 code unit 数 <= UTF-8 byte 数
    out.resize_and_overwrite(n, [text, n](wchar_t* dst, size_t) noexcept {
        const auto* const src = reinterpret_cast<const unsigned char*>(text.data());
        size_t byte_pos = 0;
        size_t wide_pos = 0;
        while (byte_pos < n) {
            if (src[byte_pos] < 0x80) {
                // 和文の中では毎回外れるので、ASCII が来たときだけ 16 byte まとめて広げ、先頭から続く ASCII の分だけ進める。
                // 広げた残りは後の書き込みで上書きされる (wide_pos <= byte_pos なので 16 個書いても n を超えない)。
                // 16KB で英文 21.5 → 0.9µs。全部 ASCII のときだけ進める形から、和英混在 18.7 → 11.9µs (BM_DecodeForDWrite)。
                if (byte_pos + 16 <= n) {
                    const __m128i c = _mm_loadu_si128(reinterpret_cast<const __m128i*>(src + byte_pos));
                    const __m128i zero = _mm_setzero_si128();
                    _mm_storeu_si128(reinterpret_cast<__m128i*>(dst + wide_pos), _mm_unpacklo_epi8(c, zero));
                    _mm_storeu_si128(reinterpret_cast<__m128i*>(dst + wide_pos + 8), _mm_unpackhi_epi8(c, zero));
                    const auto mask = static_cast<uint32_t>(_mm_movemask_epi8(c));
                    const size_t run = mask == 0 ? 16 : static_cast<size_t>(std::countr_zero(mask));
                    byte_pos += run;
                    wide_pos += run;
                    continue;
                }
                dst[wide_pos++] = static_cast<wchar_t>(src[byte_pos++]);
                continue;
            }
            const auto decoded = utf8_codec::DecodeAt(text, static_cast<uint32_t>(byte_pos));
            if (decoded.cp <= 0xFFFF) {
                dst[wide_pos++] = static_cast<wchar_t>(decoded.cp);
            }
            else {
                const uint32_t adj = decoded.cp - 0x10000u;
                dst[wide_pos++] = static_cast<wchar_t>(0xD800u + (adj >> 10));
                dst[wide_pos++] = static_cast<wchar_t>(0xDC00u + (adj & 0x3FFu));
            }
            byte_pos += decoded.len;
        }
        return wide_pos;
    });
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
    wide_ += utf8_codec::Utf16Units(decoded.cp);
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
    thread_local std::wstring wide;
    DecodeForDWrite(text, wide);
    const HRESULT hr = factory->CreateTextLayout(wide.data(), static_cast<UINT32>(wide.size()), fmt, max_w, max_h, out);
    if (wide.capacity() > kMaxRetainedChars) {
        std::wstring().swap(wide);
    }
    return hr;
}
