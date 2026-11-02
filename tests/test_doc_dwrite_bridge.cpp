#include <gtest/gtest.h>
#include "brush_id.h"
#include "d2d_util.h"
#include "doc_dwrite_bridge.h"
#include "example_files.h"
#include "theme.h"
#include "theme_palette.h"
#include "ui_constants.h"
#include <wrl/client.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <iostream>
#include <memory>
#include <ranges>
#include <string>
#include <utility>
#include <vector>

namespace {

uint32_t WideAt(std::string_view text, uint32_t byte)
{
    return Utf16OffsetCursor{ text }.WideAt(byte);
}

uint32_t ByteAt(std::string_view text, uint32_t wide)
{
    return Utf16OffsetCursor{ text }.ByteAt(wide);
}

std::wstring Decode(std::string_view text)
{
    std::wstring out;
    DecodeForDWrite(text, out);
    return out;
}

} // namespace

// ─────────────────────────────────────────────
// Utf16OffsetCursor: WideAt (byte → wide) / ByteAt (wide → byte)
// ─────────────────────────────────────────────

TEST(Utf16OffsetCursor, EmptyText)
{
    EXPECT_EQ(WideAt("", 0), 0u);
    EXPECT_EQ(ByteAt("", 0), 0u);
}

TEST(Utf16OffsetCursor, AsciiOnly)
{
    for (uint32_t i = 0; i <= 5; ++i) {
        EXPECT_EQ(WideAt("Hello", i), i);
        EXPECT_EQ(ByteAt("Hello", i), i);
    }
}

TEST(Utf16OffsetCursor, AsciiOnlyOutOfRangeClampsToSize)
{
    EXPECT_EQ(WideAt("Hello", 100), 5u);
    EXPECT_EQ(ByteAt("Hello", 100), 5u);
}

TEST(Utf16OffsetCursor, AsciiPrefixBeforeNonAscii)
{
    // "日" は 3 bytes / 1 wide
    const std::string_view text = "ab日c";
    EXPECT_EQ(WideAt(text, 1), 1u);
    EXPECT_EQ(WideAt(text, 2), 2u);
    EXPECT_EQ(WideAt(text, 5), 3u);
    EXPECT_EQ(WideAt(text, 6), 4u);
    EXPECT_EQ(ByteAt(text, 3), 5u);
}

TEST(Utf16OffsetCursor, Cjk3ByteRoundTrip)
{
    // テスト = 3 文字, UTF-8 9 bytes, UTF-16 3 wide units
    const std::string_view text = "テスト";

    EXPECT_EQ(WideAt(text, 0), 0u);
    EXPECT_EQ(WideAt(text, 3), 1u);
    EXPECT_EQ(WideAt(text, 6), 2u);
    EXPECT_EQ(WideAt(text, 9), 3u);

    // wide → byte: 対応する文字の leading byte 位置 (= continuation byte は返さない)
    EXPECT_EQ(ByteAt(text, 0), 0u);
    EXPECT_EQ(ByteAt(text, 1), 3u);
    EXPECT_EQ(ByteAt(text, 2), 6u);
    EXPECT_EQ(ByteAt(text, 3), 9u);
}

TEST(Utf16OffsetCursor, Latin2ByteRoundTrip)
{
    // é = U+00E9 (UTF-8 2 bytes, UTF-16 1 wide unit)
    // 'a'=0..1, 'é'=1..3, 'b'=3..4, 'c'=4..5 (utf8 byte / utf16 wide)
    const std::string_view text = "aébc";
    EXPECT_EQ(WideAt(text, 0), 0u);
    EXPECT_EQ(WideAt(text, 1), 1u);
    EXPECT_EQ(WideAt(text, 3), 2u);
    EXPECT_EQ(WideAt(text, 4), 3u);
    EXPECT_EQ(WideAt(text, 5), 4u);

    EXPECT_EQ(ByteAt(text, 0), 0u);
    EXPECT_EQ(ByteAt(text, 1), 1u);
    EXPECT_EQ(ByteAt(text, 2), 3u);
    EXPECT_EQ(ByteAt(text, 3), 4u);
    EXPECT_EQ(ByteAt(text, 4), 5u);
}

TEST(Utf16OffsetCursor, SurrogatePairRoundTrip)
{
    // 😀 = U+1F600 (UTF-8 4 bytes, UTF-16 2 wide units = surrogate pair)
    const std::string_view text = "A\xF0\x9F\x98\x80""B";

    EXPECT_EQ(WideAt(text, 0), 0u);
    EXPECT_EQ(WideAt(text, 1), 1u); // 絵文字 leading byte
    EXPECT_EQ(WideAt(text, 5), 3u); // 'B' leading byte
    EXPECT_EQ(WideAt(text, 6), 4u);

    // wide=2 (サロゲートペア中央) は絵文字 leading (1) ではなく次文字の leading (5)。
    EXPECT_EQ(ByteAt(text, 0), 0u);
    EXPECT_EQ(ByteAt(text, 1), 1u);
    EXPECT_EQ(ByteAt(text, 2), 5u);
    EXPECT_EQ(ByteAt(text, 3), 5u);
    EXPECT_EQ(ByteAt(text, 4), 6u);
}

// 回帰テスト: #183
// テキスト末尾の文字を選択した際、HitTest で is_trailing=true により
// wide_off = (文字数) - 1 + 1 = wide_size に達する直前の値が渡される。
// 旧実装は continuation byte の index を返してしまい、ExtractSelectedText で
// UTF-8 として不正な末尾バイト列が抽出され、貼り付け先で文字化けしていた。
TEST(Utf16OffsetCursor, EndOfNonAsciiCharNeverReturnsContinuationByte)
{
    // 末尾「ト」の手前 (= 「ス」の右端) を指す wide=2 は「ト」の leading byte (= 6)。
    EXPECT_EQ(ByteAt("テスト", 2), 6u);
    // 「テ」の右端 (= 「ス」の左端) は byte 3 (= 「ス」の leading byte)
    EXPECT_EQ(ByteAt("テスト", 1), 3u);
}

TEST(Utf16OffsetCursor, OutOfRangeClampsToBack)
{
    EXPECT_EQ(ByteAt("テスト", 3), 9u);
    EXPECT_EQ(ByteAt("テスト", 100), 9u);
    EXPECT_EQ(WideAt("テスト", 9), 3u);
    EXPECT_EQ(WideAt("テスト", 100), 3u);
}

TEST(Utf16OffsetCursor, WideRangeForCjk)
{
    // [3, 6) byte = 「ス」 = wide [1, 2)
    Utf16OffsetCursor cursor{ "テスト" };
    const auto r = cursor.WideRange(3, 3);
    EXPECT_EQ(r.startPosition, 1u);
    EXPECT_EQ(r.length, 1u);
}

// 前方へ進めたカーソルの結果は毎回作り直したカーソルと一致し (ASCII 16 byte の読み飛ばしを含む)、
// 全 byte 位置で DecodeForDWrite の変換と整合する (不正バイト含む)。
TEST(Utf16OffsetCursor, ConsistentWithDecodeForEveryOffset)
{
    std::string text = "ascii \xE3\x81\x82\xE3\x81\x84 \xF0\x9F\x98\x80 bad\xE3\x81 \xFF\x80 end";
    for (int i = 0; i < 3; ++i) {
        text += "0123456789abcdef0123456789";
    }
    text += "\xE6\x97\xA5";
    const auto wide = Decode(text);

    Utf16OffsetCursor forward{ text };
    for (uint32_t b = 0; b <= text.size(); ++b) {
        const uint32_t w = forward.WideAt(b);
        EXPECT_EQ(w, WideAt(text, b)) << "byte=" << b;
        // ByteAt は b を含む文字の先頭 byte を返し、そこから WideAt で同じ位置に戻る。
        const uint32_t lead = ByteAt(text, w);
        EXPECT_LE(lead, b) << "byte=" << b;
        EXPECT_EQ(WideAt(text, lead), w) << "byte=" << b;
    }
    EXPECT_EQ(WideAt(text, static_cast<uint32_t>(text.size())), wide.size());

    Utf16OffsetCursor forward_wide{ text };
    for (uint32_t w = 0; w <= wide.size(); ++w) {
        EXPECT_EQ(forward_wide.ByteAt(w), ByteAt(text, w)) << "wide=" << w;
    }
}

TEST(Utf16OffsetCursor, SkipsForwardWithoutVisitingEveryOffset)
{
    const std::string text = std::string(100, 'a') + "\xE3\x81\x82" + std::string(40, 'b');
    Utf16OffsetCursor cursor{ text };
    EXPECT_EQ(cursor.WideAt(50), 50u);
    EXPECT_EQ(cursor.WideAt(103), 101u);
    EXPECT_EQ(cursor.WideAt(static_cast<uint32_t>(text.size())), 141u);

    Utf16OffsetCursor back{ text };
    EXPECT_EQ(back.ByteAt(50), 50u);
    EXPECT_EQ(back.ByteAt(101), 103u);
    EXPECT_EQ(back.ByteAt(141), static_cast<uint32_t>(text.size()));
}

// ─────────────────────────────────────────────
// DecodeForDWrite
// ─────────────────────────────────────────────

TEST(DecodeForDWrite, DecodesText)
{
    EXPECT_TRUE(Decode("").empty());
    EXPECT_EQ(Decode("Hello"), L"Hello");
    EXPECT_EQ(Decode("ab日c"), L"ab日c");
    EXPECT_EQ(Decode("A\xF0\x9F\x98\x80""B"), L"A\xD83D\xDE00""B");
}

// 不正バイトは U+FFFD 1 単位。
TEST(DecodeForDWrite, InvalidBytesBecomeReplacementChar)
{
    EXPECT_EQ(Decode("a\xFF""b"), L"a\xFFFD""b");
}

// 使い回すバッファに前回の内容は残らない。
TEST(DecodeForDWrite, ReplacesPreviousContent)
{
    std::wstring out;
    DecodeForDWrite("longer text", out);
    DecodeForDWrite("ab", out);
    EXPECT_EQ(out, L"ab");
    DecodeForDWrite("", out);
    EXPECT_TRUE(out.empty());
}

// ─────────────────────────────────────────────
// FallbackFontMap: 明示してもフォールバックと同じ字形になること
// ─────────────────────────────────────────────

namespace {

using Microsoft::WRL::ComPtr;

// ランの切れ目の違いは見ないよう、グリフごとに平らにして持つ。
struct DrawnGlyph {
    std::string face;
    UINT16 index = 0;
    float x = 0.0f;
    float y = 0.0f;
    float size = 0.0f;
};

class GlyphCollector final : public UncountedCom<IDWriteTextRenderer> {
public:
    std::vector<DrawnGlyph> glyphs;

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID, void** object) override
    {
        *object = nullptr;
        return E_NOINTERFACE;
    }
    HRESULT STDMETHODCALLTYPE IsPixelSnappingDisabled(void*, BOOL* disabled) override
    {
        *disabled = TRUE;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetCurrentTransform(void*, DWRITE_MATRIX* transform) override
    {
        *transform = { 1, 0, 0, 1, 0, 0 };
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetPixelsPerDip(void*, FLOAT* pixels) override
    {
        *pixels = 1.0f;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE DrawUnderline(void*, FLOAT, FLOAT, const DWRITE_UNDERLINE*, IUnknown*) override
    {
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE DrawStrikethrough(void*, FLOAT, FLOAT, const DWRITE_STRIKETHROUGH*, IUnknown*) override
    {
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE DrawInlineObject(void*, FLOAT, FLOAT, IDWriteInlineObject*, BOOL, BOOL, IUnknown*) override
    {
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE DrawGlyphRun(void*, FLOAT x, FLOAT y, DWRITE_MEASURING_MODE, const DWRITE_GLYPH_RUN* run, const DWRITE_GLYPH_RUN_DESCRIPTION*, IUnknown*) override
    {
        UINT32 files = 1;
        ComPtr<IDWriteFontFile> file;
        run->fontFace->GetFiles(&files, &file);
        const void* key = nullptr;
        UINT32 key_size = 0;
        file->GetReferenceKey(&key, &key_size);
        std::string face(static_cast<const char*>(key), key_size);
        face += std::to_string(run->fontFace->GetIndex()) + ":" + std::to_string(static_cast<int>(run->fontFace->GetSimulations()));
        // 右から左のランは x が右端で、ペンは左へ進む。advanceOffset も進む向きに掛かる。
        const bool rtl = (run->bidiLevel & 1) != 0;
        float pen = x;
        for (UINT32 i = 0; i < run->glyphCount; ++i) {
            const float advance = run->glyphAdvances[i];
            const DWRITE_GLYPH_OFFSET offset = run->glyphOffsets ? run->glyphOffsets[i] : DWRITE_GLYPH_OFFSET{};
            const float origin = rtl ? pen - advance - offset.advanceOffset : pen + offset.advanceOffset;
            glyphs.push_back({ face, run->glyphIndices[i], origin, y - offset.ascenderOffset, run->fontEmSize });
            pen += rtl ? -advance : advance;
        }
        return S_OK;
    }
};

struct Style {
    DWRITE_FONT_WEIGHT weight = DWRITE_FONT_WEIGHT_NORMAL;
    bool italic = false;
};

class FallbackFontMapTest : public ::testing::Test {
protected:
    void SetUp() override
    {
        ASSERT_TRUE(SUCCEEDED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory), reinterpret_cast<IUnknown**>(factory_.GetAddressOf()))));
    }

    ComPtr<IDWriteTextLayout> Build(std::wstring_view text, IDWriteTextFormat* format, const Style& style, const FallbackFontMap* fonts) const
    {
        ComPtr<IDWriteTextLayout> layout;
        factory_->CreateTextLayout(text.data(), static_cast<UINT32>(text.size()), format, 300.0f, 1.0e7f, &layout);
        if (fonts) {
            ApplyFallbackFonts(layout.Get(), text, *fonts);
        }
        const DWRITE_TEXT_RANGE all{ 0, static_cast<UINT32>(text.size()) };
        layout->SetFontWeight(style.weight, all);
        if (style.italic) {
            layout->SetFontStyle(DWRITE_FONT_STYLE_ITALIC, all);
        }
        return layout;
    }

    // どのフォントにも字形が無い文字 (未割り当てなど) を含む。
    bool HasMissingGlyph(std::wstring_view text, IDWriteTextFormat* format) const
    {
        GlyphCollector plain;
        Build(text, format, {}, nullptr)->Draw(nullptr, &plain, 0.0f, 0.0f);
        return std::ranges::any_of(plain.glyphs, [](const DrawnGlyph& g) static { return g.index == 0; });
    }

    // 明示の有無でグリフ・位置・行の高さが同じか。
    ::testing::AssertionResult SameAsFallback(std::wstring_view text, IDWriteTextFormat* format, const FallbackFontMap& fonts, const Style& style = {}) const
    {
        const auto plain = Build(text, format, style, nullptr);
        const auto hinted = Build(text, format, style, &fonts);
        GlyphCollector a;
        GlyphCollector b;
        plain->Draw(nullptr, &a, 0.0f, 0.0f);
        hinted->Draw(nullptr, &b, 0.0f, 0.0f);
        DWRITE_TEXT_METRICS ma{};
        DWRITE_TEXT_METRICS mb{};
        plain->GetMetrics(&ma);
        hinted->GetMetrics(&mb);
        if (ma.height != mb.height || ma.lineCount != mb.lineCount || std::abs(ma.widthIncludingTrailingWhitespace - mb.widthIncludingTrailingWhitespace) > 0.01f) {
            return ::testing::AssertionFailure() << "metrics differ: height " << ma.height << " / " << mb.height << ", lines " << ma.lineCount << " / " << mb.lineCount;
        }
        if (a.glyphs.size() != b.glyphs.size()) {
            return ::testing::AssertionFailure() << "glyph count " << a.glyphs.size() << " / " << b.glyphs.size();
        }
        for (size_t i = 0; i < a.glyphs.size(); ++i) {
            const auto& x = a.glyphs[i];
            const auto& y = b.glyphs[i];
            if (x.face != y.face || x.index != y.index || x.size != y.size || std::abs(x.x - y.x) > 0.01f || std::abs(x.y - y.y) > 0.01f) {
                return ::testing::AssertionFailure() << "glyph " << i << " differs (index " << x.index << " / " << y.index << ", x " << x.x << " / " << y.x << ")";
            }
        }
        return ::testing::AssertionSuccess();
    }

    ComPtr<IDWriteTextFormat> Format(const wchar_t* family, const wchar_t* locale, float line_height_factor = BODY_LINE_HEIGHT_FACTOR) const
    {
        return CreateTextFormat(factory_.Get(), { .family = family, .size = 16.0f, .locale = locale, .line_height_factor = line_height_factor });
    }

    std::unique_ptr<FallbackFontMap> Map(const wchar_t* family, const wchar_t* locale) const
    {
        auto map = std::make_unique<FallbackFontMap>();
        map->Reset(factory_.Get(), family, locale);
        return map;
    }

    ComPtr<IDWriteFactory> factory_;
};

// 本文の主フォントと、コードの主フォントになりうる候補。
constexpr auto kBases = [] {
    std::array<const wchar_t*, 1 + std::size(theme_palette::kMonospaceFontCandidates)> bases{ L"Segoe UI" };
    std::ranges::copy(theme_palette::kMonospaceFontCandidates, bases.begin() + 1);
    return bases;
}();
// 太字の run は EXTRA_BOLD で組む。見出しの書式は BOLD。
constexpr Style kStyles[]{ {}, { DWRITE_FONT_WEIGHT_BOLD }, { DWRITE_FONT_WEIGHT_EXTRA_BOLD }, { DWRITE_FONT_WEIGHT_NORMAL, true } };
constexpr const wchar_t* kLocales[]{ L"ja-jp", L"zh-cn", L"zh-tw", L"ko-kr", L"en-us", L"th-th", L"hi-in", L"ar-sa", L"ru-ru" };

// 文字体系ごとの文章と、区間の境目で字形が変わりやすい並び。
constexpr const wchar_t* kCorpus[]{
    L"吾輩は猫である。名前はまだ無い。どこで生れたかとんと見当がつかぬ。「カタカナ」も全角ＡＢＣも半角ｶﾞｷﾞも混ぜる。",
    L"本文は Direct2D と DirectWrite で描く Markdown ビューアーで、layout_cache に結果を保持する。",
    L"应用程序使用直接绘制高速显示内容。繁體字也一併顯示。",
    L"나는 고양이다. 이름은 아직 없다. 한글과 English 섞기.",
    L"ประเทศไทยมีภาษาราชการคือภาษาไทย",
    L"ພາສາລາວແມ່ນພາສາທາງການ",
    L"हिन्दी भारत की एक प्रमुख भाषा है। क्ष ज्ञ श्र",
    L"বাংলা ভাষা দক্ষিণ এশিয়ার একটি প্রধান ভাষা",
    L"தமிழ் மொழி உலகின் மிகப் பழமையான மொழிகளில் ஒன்றாகும்",
    L"తెలుగు ಕನ್ನಡ മലയാളം සිංහල ગુજરાતી ਪੰਜਾਬੀ ଓଡ଼ିଆ",
    L"မြန်မာဘာသာစကား ភាសាខ្មែរ አማርኛ",
    L"Русский Ελληνικά العربية עברית Հայերեն ქართული",
    L"波\u3030\uFE0F線 印\u303D\uFE0F記 祝\u3297\uFE0F賀",
    L"漢\uFE00字 葛\xDB40\xDD00飾区 辻\xDB40\xDD01本",
    L"か\u3099き\u309Aく あ\u0301い 漢\u20DD字",
    L"a\u3099b a\u0E31b a\u094Db a\u0BCDb",
    L"क\u094D\u200Dष क\u200Cष ক\u09CD\u200Dষ",
    L"𠮷野家𩸽 漢\u200D字 日本😀語👍🏽文 番号1\uFE0F\u20E3です",
    L"「…—×→●★①㈱㌔」㊗㊙🈁🈂 Ａ\uFE00Ｂ",
    L"\u1112\u1161\u11ab글 日本語 العربية עברית 日本語",
    L"日本語\n次の行\r\n三行目\u2028四行目\t次",
    L"漢\uE000字",
    // 肌の色の修飾子や ZWJ で 1 つの字形になる絵文字は、和文の隣でも分かれない。単独の修飾子はどのフォントにも無い。
    L"漢\U0001F9D7\U0001F3FF字\U0001F3F3\uFE0F\u200D\U0001F308語\U0001F468\u200D\U0001F469\u200D\U0001F467文\U0001F3FF字",
};

} // namespace

// 区間の数には上限があり、上限を超えた分はフォールバックに任せる。字形はどちらでも同じ。
TEST_F(FallbackFontMapTest, GlyphsMatchFallbackBeyondRangeLimit)
{
    std::wstring text;
    for (int i = 0; i < 2000; ++i) {
        text += static_cast<wchar_t>(0x4E00 + i);
        text += L'a';
    }
    const auto format = Format(L"Segoe UI", L"ja-jp");
    const auto map = Map(L"Segoe UI", L"ja-jp");
    EXPECT_TRUE(SameAsFallback(text, format.Get(), *map));
}

TEST_F(FallbackFontMapTest, UnknownScriptsAreLeftToTheSystem)
{
    const auto map = Map(L"Segoe UI", L"ja-jp");
    EXPECT_EQ(map->FamilyFor(L'a'), nullptr);
    EXPECT_EQ(map->FamilyFor(L'Д'), nullptr);
    EXPECT_EQ(map->FamilyFor(L'ع'), nullptr);
    EXPECT_EQ(map->FamilyFor(L'\xE000'), nullptr);
    EXPECT_EQ(map->FamilyFor(L'\xD83D'), nullptr);
}

// フォントを尋ねられなければ、どの文字も今までどおりフォールバックに任せる。
TEST_F(FallbackFontMapTest, NoFactoryLeavesEverythingToTheSystem)
{
    FallbackFontMap map;
    map.Reset(nullptr, L"Segoe UI", L"ja-jp");
    EXPECT_EQ(map.FamilyFor(L'漢'), nullptr);
    EXPECT_EQ(map.FamilyFor(L'한'), nullptr);
}

// 同じフォントの文字は同じポインタを返し、続けて 1 つの区間に掛けられる。
TEST_F(FallbackFontMapTest, SameFontSharesTheName)
{
    const auto map = Map(L"Segoe UI", L"ja-jp");
    const wchar_t* const han = map->FamilyFor(L'漢');
    if (!han) {
        GTEST_SKIP() << "この環境には日本語のフォントが無い";
    }
    EXPECT_EQ(map->FamilyFor(L'字'), han);
    EXPECT_EQ(map->FamilyFor(L'か'), han);
    EXPECT_EQ(map->FamilyFor(L'。'), han);
}

// 言語設定を変えたら引き直す。
TEST_F(FallbackFontMapTest, ResetChangesLocale)
{
    const auto name_of = [](const wchar_t* family) static { return std::wstring(family ? family : L""); };
    FallbackFontMap map;
    map.Reset(factory_.Get(), L"Segoe UI", L"ja-jp");
    map.FamilyFor(L'漢');
    map.Reset(factory_.Get(), L"Segoe UI", L"zh-cn");
    // 選ばれるフォントは環境で変わるので、言語の間で違うことは求めない。
    const auto fresh = Map(L"Segoe UI", L"zh-cn");
    EXPECT_EQ(name_of(map.FamilyFor(L'漢')), name_of(fresh->FamilyFor(L'漢')));
}

// 書式のフォントに字形がある文字体系は明示しない (速くならない)。
TEST_F(FallbackFontMapTest, BaseFontIsNotNamed)
{
    const auto map = Map(L"Yu Gothic UI", L"ja-jp");
    EXPECT_EQ(map->FamilyFor(L'か'), nullptr);
}

TEST_F(FallbackFontMapTest, GlyphsMatchFallbackForEveryLanguage)
{
    for (const auto [base, locale] : std::views::cartesian_product(kBases, kLocales)) {
        const auto format = Format(base, locale);
        const auto map = Map(base, locale);
        for (const auto [text, style] : std::views::cartesian_product(kCorpus, kStyles)) {
            EXPECT_TRUE(SameAsFallback(text, format.Get(), *map, style)) << "base=" << testing::PrintToString(std::wstring(base)) << " locale=" << testing::PrintToString(std::wstring(locale)) << " text=" << testing::PrintToString(std::wstring(text)) << " weight=" << style.weight << " italic=" << style.italic;
        }
    }
}

// ファイル一覧と目次の項目は、本文の対応を本文と違う大きさと NO_WRAP の書式に掛ける。大きさはペインのズームの両端と等倍。
TEST_F(FallbackFontMapTest, GlyphsMatchFallbackForPaneItems)
{
    constexpr const wchar_t* kTextLocales[]{ L"ja-jp", L"zh-cn", L"zh-tw", L"zh-hk", L"ko-kr" };
    const float pane_size = GetLightTheme().pane_font_size;
    for (const auto [zoom, locale] : std::views::cartesian_product(std::array{ ZOOM_STEPS[0], 1.0f, ZOOM_STEPS[ZOOM_STEP_COUNT - 1] }, kTextLocales)) {
        const float size = pane_size * zoom;
        const auto format = CreateTextFormat(factory_.Get(), { .family = L"Segoe UI", .size = size, .locale = locale, .para_align = DWRITE_PARAGRAPH_ALIGNMENT_CENTER, .no_wrap = true });
        const auto map = Map(L"Segoe UI", locale);
        for (const wchar_t* const text : kCorpus) {
            EXPECT_TRUE(SameAsFallback(text, format.Get(), *map)) << "size=" << size << " locale=" << testing::PrintToString(std::wstring(locale)) << " text=" << testing::PrintToString(std::wstring(text));
        }
    }
}

TEST_F(FallbackFontMapTest, GlyphsMatchFallbackForRealDocument)
{
    const auto bytes = ReadExampleFileBytes("example/test.md");
    ASSERT_FALSE(bytes.empty());
    std::wstring all;
    DecodeForDWrite(bytes, all);
    for (const auto* base : kBases) {
        const auto format = Format(base, L"ja-jp");
        const auto map = Map(base, L"ja-jp");
        for (const auto line : all | std::views::split(L'\n')) {
            const std::wstring_view text(line.begin(), line.end());
            if (!text.empty()) {
                ASSERT_TRUE(SameAsFallback(text, format.Get(), *map)) << testing::PrintToString(std::wstring(text));
            }
        }
    }
}

// 対応表の全文字を 1 文字ずつ比べる。時間がかかるので、対応表を変えたときに --gtest_also_run_disabled_tests で回す。
// どのフォントにも字形が無い文字 (未割り当てなど) は豆腐の形しか変わらないので比べない。
// 英字に挟まれた文字と、和文に挟まれた文字の両方を見る。前後の文字でフォールバック先が変わることがある。
TEST_F(FallbackFontMapTest, DISABLED_GlyphsMatchFallbackForEveryCharacter)
{
    constexpr std::pair<wchar_t, wchar_t> kNeighbours[]{ { L'a', L'b' }, { L'漢', L'字' } };
    for (const auto [base, locale] : std::views::cartesian_product(kBases, kLocales)) {
        const auto format = Format(base, locale, 0.0f);
        const auto map = Map(base, locale);
        size_t mismatches = 0;
        for (UINT32 c = 0x0900; c <= 0xFFEF; ++c) {
            if (!map->FamilyFor(static_cast<wchar_t>(c))) {
                continue;
            }
            for (const auto& [before, after] : kNeighbours) {
                const wchar_t text[]{ before, static_cast<wchar_t>(c), after };
                if (HasMissingGlyph({ text, 3 }, format.Get())) {
                    continue;
                }
                for (const Style& style : kStyles) {
                    if (SameAsFallback({ text, 3 }, format.Get(), *map, style)) {
                        continue;
                    }
                    if (++mismatches <= 40) {
                        ADD_FAILURE() << std::hex << "U+" << c << " " << testing::PrintToString(std::wstring(base)) << " " << testing::PrintToString(std::wstring(locale)) << " weight=" << std::dec << style.weight << " italic=" << style.italic;
                    }
                }
            }
        }
        std::wcout << base << L" " << locale << L": " << mismatches << L" mismatches\n";
    }
}
