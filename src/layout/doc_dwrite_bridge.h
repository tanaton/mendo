#pragma once
#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <dwrite.h>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>
#include <wrl/client.h>

// Document テキスト (UTF-8 string_view) を IDWriteTextLayout / IDWriteTextFormat に
// 渡すための境界ラッパー。MultiByteToWideChar 相当の自前 decode で UTF-16 にし、
// UTF-8 byte と UTF-16 code unit の位置は Utf16OffsetCursor で前方に数えて対応させる。
//
// 描画用 wchar_t (フォント名 / locale / i18n リテラル等) はこの境界の対象外。
// それらは std::wstring 経路を維持し、CreateDocTextLayout を経由しない。

namespace utf8_codec {
struct DecodedCp;
}

// UTF-8 byte 位置と UTF-16 位置を前方へ数えて対応させるカーソル。DecodeForDWrite と同じ
// decode 規則 (不正バイトは U+FFFD 1 単位、sequence 途中の byte はその文字の直前位置) に従う。
// 全文を UTF-16 化しないので、巨大ノードでもメモリを確保しない。
// WideAt / ByteAt に渡す位置は、どちらの単位で見ても単調非減少であること。
class Utf16OffsetCursor {
public:
    explicit Utf16OffsetCursor(std::string_view text) noexcept : text_(text)
    {}

    // 範囲外は末尾に clamping。
    uint32_t WideAt(uint32_t byte_target) noexcept;

    // wide_target から始まる文字の先頭 byte (continuation byte は返さない)。サロゲートペアの
    // 途中を指すなら次の文字の先頭。範囲外は末尾に clamping。
    uint32_t ByteAt(uint32_t wide_target) noexcept;

    DWRITE_TEXT_RANGE WideRange(uint32_t byte_start, uint32_t byte_length) noexcept
    {
        const uint32_t w_start = WideAt(byte_start);
        return DWRITE_TEXT_RANGE{ w_start, WideAt(byte_start + byte_length) - w_start };
    }

private:
    // byte_ から 16 byte が ASCII なら 1:1 対応として読み飛ばす。
    bool SkipAscii16() noexcept;
    void Advance(const utf8_codec::DecodedCp& decoded) noexcept;

    std::string_view text_;
    uint32_t byte_ = 0;
    uint32_t wide_ = 0;
};

// out の内容は置き換え、容量は再利用する。
void DecodeForDWrite(std::string_view text, std::wstring& out);

// HitTestTextRange をバッファ再利用付きで呼び出し、取得件数を返す。
inline UINT32 FetchHitTestMetrics(IDWriteTextLayout* layout, UINT32 start, UINT32 length, std::vector<DWRITE_HIT_TEST_METRICS>& buffer)
{
    // 1 行中の inline code run が折り返される想定最大数。描画 hot path 中の resize を避ける。
    constexpr size_t kInitialCapacity = 64;
    if (buffer.size() < kInitialCapacity) {
        buffer.resize(kInitialCapacity);
    }
    UINT32 count = static_cast<UINT32>(buffer.size());
    HRESULT hr = layout->HitTestTextRange(start, length, 0, 0, buffer.data(), count, &count);
    if (hr == E_NOT_SUFFICIENT_BUFFER) {
        buffer.resize(count);
        layout->HitTestTextRange(start, length, 0, 0, buffer.data(), count, &count);
    }
    return count;
}

// [start, start+length) を、両端を含むクラスタの境目まで広げる。インライン数式は 1 つのクラスタで、途中だけを指す範囲
// (数式の中の検索ヒット) は HitTestTextRange が幅 0 の箱を返し、頭だけを含む範囲では数式を除いた箱を返す。
inline DWRITE_TEXT_RANGE ExpandToClusters(IDWriteTextLayout* layout, UINT32 start, UINT32 length)
{
    if (length == 0) {
        return { start, length };
    }
    FLOAT x = 0.0f;
    FLOAT y = 0.0f;
    DWRITE_HIT_TEST_METRICS first{};
    DWRITE_HIT_TEST_METRICS last{};
    if (FAILED(layout->HitTestTextPosition(start, FALSE, &x, &y, &first)) ||
        FAILED(layout->HitTestTextPosition(start + length - 1, FALSE, &x, &y, &last))) {
        return { start, length };
    }
    const UINT32 begin = std::min(start, first.textPosition);
    const UINT32 end = std::max(start + length, last.textPosition + last.length);
    return { begin, end - begin };
}

// pos を含む行全体の箱。行間を足した layout では HitTestPoint / HitTestTextPosition の箱は
// 足した余白を除くので、行の上端と高さは長さ 0 の HitTestTextRange で引く。
inline std::optional<DWRITE_HIT_TEST_METRICS> LineBoxAt(IDWriteTextLayout* layout, UINT32 pos)
{
    DWRITE_HIT_TEST_METRICS m{};
    UINT32 count = 0;
    if (FAILED(layout->HitTestTextRange(pos, 0, 0.0f, 0.0f, &m, 1, &count)) || count == 0) {
        return std::nullopt;
    }
    return m;
}

// cp を描く書体をシステムのフォールバックに尋ねる。base_family はフォールバックの元にする書体。見つからなければ null。
// scale は書体の大きさに掛ける倍率 (要るときだけ)。
Microsoft::WRL::ComPtr<IDWriteFont> MapSystemFallback(IDWriteFactory* factory, uint32_t cp, const wchar_t* base_family, DWRITE_FONT_WEIGHT weight,
                                                      DWRITE_FONT_STYLE style, const wchar_t* locale, float* scale = nullptr);

// 書式のフォントに字形が無い文字体系の文字を、システムのフォールバックが選ぶフォント名に対応させる。
// 計測の手間の大半はフォールバックの処理で、同じフォントを範囲に明示するとグリフは同じまま速くなる
// (test.md の幅変更 192 → 56ms)。選ばれるフォントは言語設定と環境で変わる (同じ漢字が ko-kr では Malgun Gothic) ので、
// 名前を持たずシステムに尋ねる。解決には 1 文字体系で数百 µs かかるので、文書に現れた文字体系だけを初めて現れたときに解決する。
// kScripts に無い文字体系、尋ねても分からないもの、書式のフォントに字形がある文字はこれまでどおりに組むので、
// どの言語の文字も表示は変わらない。
class FallbackFontMap {
public:
    static constexpr size_t kScriptCount = 21;

    // base_family を主フォントとする locale の書式の対応に切り替える。計測中に呼ばないこと。
    void Reset(IDWriteFactory* factory, std::wstring_view base_family, std::wstring_view locale);
    // c に明示するフォント名。無ければ nullptr。同じフォントなら同じポインタを返す。計測ワーカーから同時に呼べる。
    const wchar_t* FamilyFor(wchar_t c) const;

private:
    const wchar_t* Resolve(size_t script) const;

    Microsoft::WRL::ComPtr<IDWriteFactory> factory_;
    // 最初の Resolve が決める。resolved_ を acquire で読んだ後なら参照できる。
    mutable Microsoft::WRL::ComPtr<IDWriteFont> base_font_;
    std::wstring base_family_;
    std::wstring locale_;
    mutable std::array<std::atomic<const wchar_t*>, kScriptCount> resolved_{};
    mutable std::mutex mutex_;
    mutable std::array<std::wstring, kScriptCount> names_;
};

// text を組んだ layout の、フォントが分かる文字の区間に SetFontFamilyName を掛ける。
void ApplyFallbackFonts(IDWriteTextLayout* layout, std::wstring_view text, const FallbackFontMap& fonts);

HRESULT CreateDocTextLayout(
    IDWriteFactory* factory, std::string_view text,
    IDWriteTextFormat* fmt, float max_w, float max_h,
    const FallbackFontMap& fonts, IDWriteTextLayout** out);
