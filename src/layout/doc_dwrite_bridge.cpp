#include "doc_dwrite_bridge.h"
#include "brush_id.h"
#include "utf8_codec.h"
#include <algorithm>
#include <bit>
#include <dwrite_2.h>
#include <emmintrin.h>
#include <iterator>

using Microsoft::WRL::ComPtr;

namespace {

// Segoe UI に字形が無い文字体系。sample はシステムのフォールバックに尋ねる代表の文字で、その文字のフォントを区間に明示する。
// 区間の文字が明示したフォントに無くても、そこからのフォールバックで今までと同じ字形になる。
// 文字体系として無くても、Indic の数字やタイのバーツ記号のように書式のフォントにある文字は FamilyFor が除く。
// Segoe UI にあるキリル・ギリシャ・アラビア・ヘブライ文字などは載せない。明示すると遅くなる (+50%)。
struct ScriptRange {
    wchar_t first;
    wchar_t last;
    wchar_t sample;
};
constexpr ScriptRange kScripts[]{
    { 0x0900, 0x097F, L'\u0915' }, // デーヴァナーガリー
    { 0x0980, 0x09FF, L'\u0995' }, // ベンガル
    { 0x0A00, 0x0A7F, L'\u0A15' }, // グルムキー
    { 0x0A80, 0x0AFF, L'\u0A95' }, // グジャラート
    { 0x0B00, 0x0B7F, L'\u0B15' }, // オリヤー
    { 0x0B80, 0x0BFF, L'\u0BA4' }, // タミル
    { 0x0C00, 0x0C7F, L'\u0C15' }, // テルグ
    { 0x0C80, 0x0CFF, L'\u0C95' }, // カンナダ
    { 0x0D00, 0x0D7F, L'\u0D15' }, // マラヤーラム
    { 0x0D80, 0x0DFF, L'\u0D9A' }, // シンハラ
    { 0x0E00, 0x0E7F, L'\u0E01' }, // タイ
    { 0x0E80, 0x0EFF, L'\u0E81' }, // ラオ
    { 0x1000, 0x109F, L'\u1000' }, // ミャンマー
    { 0x1200, 0x137F, L'\u12A0' }, // エチオピア
    { 0x1780, 0x17FF, L'\u1780' }, // クメール
    { 0x3000, 0x303F, L'\u3001' }, // CJK の記号
    { 0x3040, 0x30FF, L'\u3042' }, // かな
    // 拡張 A は個々の文字に尋ねるとフォントが割れる (ja-jp で大半が JhengHei)。漢字のフォントを明示すれば、無い文字は同じ先へ落ちる。
    { 0x3400, 0x4DBF, L'\u6F22' },
    { 0x4E00, 0x9FFF, L'\u6F22' },
    { 0xAC00, 0xD7A3, L'\uD55C' }, // ハングル
    { 0xFF00, 0xFFEF, L'\uFF21' }, // 全角・半角
};
static_assert(std::ranges::is_sorted(kScripts, {}, &ScriptRange::first));

// 異体字セレクタ・結合文字・絵文字の VS16 は前の文字と、ゼロ幅接合子は前後の文字と同じフォントで組まれるので、
// 端の文字だけを明示すると字形が変わる。種類を見分けず、これ以上の対応の無い文字の両隣はフォールバックに残す
// (その 1 文字が速くならないだけ)。下限は結合文字のブロックの先頭。
constexpr wchar_t kFirstCombiningChar = 0x0300;

// 1 つのレイアウトに明示する区間の数の上限。SetFontFamilyName は区間の数の二乗で遅くなり、和文と英字が 1 字ずつ交互に並ぶ
// 18KB のノード (4500 区間) では明示しないときの 60ms が全部明示すると 101ms になる。上限までなら 55ms で、
// どの区間数でも明示しないより遅くならない。普通の和文は 16KB のノードでも数百区間なので届かない。
// 掛ける順序は前からのままにする。後ろからだと同じ 4500 区間が 311ms になる。
constexpr size_t kMaxFallbackRanges = 1024;

// 1 文字をシステムのフォールバックに尋ねるための入力。
class SingleCharSource final : public UncountedCom<IDWriteTextAnalysisSource> {
public:
    SingleCharSource(wchar_t text, const wchar_t* locale) noexcept : text_(text), locale_(locale)
    {}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID, void** object) override
    {
        *object = nullptr;
        return E_NOINTERFACE;
    }
    HRESULT STDMETHODCALLTYPE GetTextAtPosition(UINT32 position, const WCHAR** text, UINT32* length) override
    {
        *text = position == 0 ? &text_ : nullptr;
        *length = position == 0 ? 1 : 0;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetTextBeforePosition(UINT32 position, const WCHAR** text, UINT32* length) override
    {
        *text = position > 0 ? &text_ : nullptr;
        *length = position > 0 ? 1 : 0;
        return S_OK;
    }
    DWRITE_READING_DIRECTION STDMETHODCALLTYPE GetParagraphReadingDirection() override
    {
        return DWRITE_READING_DIRECTION_LEFT_TO_RIGHT;
    }
    HRESULT STDMETHODCALLTYPE GetLocaleName(UINT32, UINT32* length, const WCHAR** locale) override
    {
        *length = 1;
        *locale = locale_;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetNumberSubstitution(UINT32, UINT32* length, IDWriteNumberSubstitution** substitution) override
    {
        *length = 1;
        *substitution = nullptr;
        return S_OK;
    }

private:
    wchar_t text_;
    const wchar_t* locale_;
};

std::wstring FamilyNameOf(IDWriteFont* font)
{
    ComPtr<IDWriteFontFamily> family;
    ComPtr<IDWriteLocalizedStrings> names;
    if (FAILED(font->GetFontFamily(&family)) || FAILED(family->GetFamilyNames(&names))) {
        return {};
    }
    UINT32 index = 0;
    BOOL exists = FALSE;
    names->FindLocaleName(L"en-us", &index, &exists);
    index = exists ? index : 0;
    UINT32 length = 0;
    if (FAILED(names->GetStringLength(index, &length))) {
        return {};
    }
    std::wstring name(length + 1, L'\0');
    if (FAILED(names->GetString(index, name.data(), length + 1))) {
        return {};
    }
    name.resize(length);
    return name;
}

ComPtr<IDWriteFont> FindFont(IDWriteFactory* factory, const std::wstring& family)
{
    ComPtr<IDWriteFontCollection> collection;
    ComPtr<IDWriteFontFamily> found;
    ComPtr<IDWriteFont> font;
    UINT32 index = 0;
    BOOL exists = FALSE;
    if (!factory || FAILED(factory->GetSystemFontCollection(&collection)) || FAILED(collection->FindFamilyName(family.c_str(), &index, &exists)) || !exists ||
        FAILED(collection->GetFontFamily(index, &found)) ||
        FAILED(found->GetFirstMatchingFont(DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STRETCH_NORMAL, DWRITE_FONT_STYLE_NORMAL, &font))) {
        return nullptr;
    }
    return font;
}

// 分からなければ空。大きさを変えて組むフォント (scale が 1 でない) は名前だけでは再現できないので空にする。
std::wstring AskSystemFallback(IDWriteFactory* factory, wchar_t sample, const std::wstring& base_family, const std::wstring& locale)
{
    ComPtr<IDWriteFactory2> factory2;
    ComPtr<IDWriteFontFallback> fallback;
    ComPtr<IDWriteFontCollection> collection;
    if (FAILED(factory->QueryInterface(IID_PPV_ARGS(&factory2))) || FAILED(factory2->GetSystemFontFallback(&fallback)) ||
        FAILED(factory->GetSystemFontCollection(&collection))) {
        return {};
    }
    SingleCharSource source(sample, locale.c_str());
    UINT32 mapped_length = 0;
    ComPtr<IDWriteFont> font;
    FLOAT scale = 0.0f;
    const HRESULT hr = fallback->MapCharacters(&source, 0, 1, collection.Get(), base_family.c_str(), DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL,
                                               DWRITE_FONT_STRETCH_NORMAL, &mapped_length, &font, &scale);
    if (FAILED(hr) || !font || mapped_length != 1 || scale != 1.0f) {
        return {};
    }
    return FamilyNameOf(font.Get());
}

// 解決した結果「明示しない」を表す。FamilyFor の nullptr (未解決) と区別する。
const wchar_t* const kNoFamily = L"";

} // namespace

static_assert(std::size(kScripts) == FallbackFontMap::kScriptCount);

void FallbackFontMap::Reset(IDWriteFactory* factory, std::wstring_view base_family, std::wstring_view locale)
{
    // ズームのたびに呼ばれるが、対応は文字の大きさに依らない。解決した分を捨てない。
    if (factory_.Get() == factory && base_family_ == base_family && locale_ == locale) {
        return;
    }
    factory_ = factory;
    base_font_.Reset();
    base_family_ = base_family;
    locale_ = locale;
    for (auto& resolved : resolved_) {
        resolved.store(nullptr, std::memory_order_relaxed);
    }
    names_ = {};
}

const wchar_t* FallbackFontMap::FamilyFor(wchar_t c) const
{
    if (c < kScripts[0].first) {
        return nullptr;
    }
    const auto* const it = std::ranges::lower_bound(kScripts, c, {}, &ScriptRange::last);
    if (it == std::end(kScripts) || c < it->first) {
        return nullptr;
    }
    const auto script = static_cast<size_t>(it - std::begin(kScripts));
    const wchar_t* family = resolved_[script].load(std::memory_order_acquire);
    if (!family) {
        family = Resolve(script);
    }
    if (family == kNoFamily) {
        return nullptr;
    }
    BOOL has_glyph = TRUE;
    base_font_->HasCharacter(c, &has_glyph);
    return has_glyph ? nullptr : family;
}

const wchar_t* FallbackFontMap::Resolve(size_t script) const
{
    const std::lock_guard lock(mutex_);
    if (const wchar_t* const done = resolved_[script].load(std::memory_order_relaxed)) {
        return done;
    }
    if (!base_font_) {
        base_font_ = FindFont(factory_.Get(), base_family_);
    }
    // 書式のフォントが分からなければ、どの文字に字形があるかも分からないので明示しない。
    auto name = base_font_ ? AskSystemFallback(factory_.Get(), kScripts[script].sample, base_family_, locale_) : std::wstring{};
    const wchar_t* family = kNoFamily;
    if (!name.empty() && name != base_family_) {
        // 同じフォントの文字体系は同じポインタを返す。別のポインタだと区間が途中で切れ、切れ目の字形が変わる。
        const auto it = std::ranges::find(names_, name);
        family = (it != names_.end() ? *it : names_[script] = std::move(name)).c_str();
    }
    resolved_[script].store(family, std::memory_order_release);
    return family;
}

void ApplyFallbackFonts(IDWriteTextLayout* layout, std::wstring_view text, const FallbackFontMap& fonts)
{
    size_t ranges = 0;
    for (size_t i = 0; i < text.size() && ranges < kMaxFallbackRanges;) {
        const wchar_t* const family = fonts.FamilyFor(text[i]);
        if (!family) {
            ++i;
            continue;
        }
        size_t next = i + 1;
        while (next < text.size() && fonts.FamilyFor(text[next]) == family) {
            ++next;
        }
        const size_t begin = i > 0 && text[i - 1] >= kFirstCombiningChar ? i + 1 : i;
        const size_t end = next < text.size() && text[next] >= kFirstCombiningChar ? next - 1 : next;
        if (begin < end) {
            layout->SetFontFamilyName(family, { static_cast<UINT32>(begin), static_cast<UINT32>(end - begin) });
            ++ranges;
        }
        i = next;
    }
}

// MultiByteToWideChar は不正バイトの扱いが OS 依存なので、Utf16OffsetCursor と同じ規則の自前 decode に揃える。
void DecodeForDWrite(std::string_view text, std::wstring& out)
{
    out.clear();
    if (text.empty()) {
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
    const FallbackFontMap& fonts, IDWriteTextLayout** out)
{
    constexpr size_t kMaxRetainedChars = 8 * 1024;
    thread_local std::wstring wide;
    DecodeForDWrite(text, wide);
    const HRESULT hr = factory->CreateTextLayout(wide.data(), static_cast<UINT32>(wide.size()), fmt, max_w, max_h, out);
    if (SUCCEEDED(hr)) {
        ApplyFallbackFonts(*out, wide, fonts);
    }
    if (wide.capacity() > kMaxRetainedChars) {
        std::wstring().swap(wide);
    }
    return hr;
}
