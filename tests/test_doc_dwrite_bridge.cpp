#include <gtest/gtest.h>
#include "brush_id.h"
#include "d2d_util.h"
#include "doc_dwrite_bridge.h"
#include "example_files.h"
#include "task_scheduler.h"
#include "test_helpers.h"
#include "theme.h"
#include "theme_palette.h"
#include "theme_service.h"
#include "ui_constants.h"
#include <wrl/client.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <format>
#include <iostream>
#include <memory>
#include <ranges>
#include <string>
#include <string_view>
#include <thread>
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

// 文字列ごとの WideAt (byte → wide) と ByteAt (wide → byte) の対応。範囲外は末尾に丸める。
TEST(Utf16OffsetCursor, MapsOffsetsBothWays)
{
    using Pairs = std::vector<std::pair<uint32_t, uint32_t>>;
    struct Case {
        std::string_view text;
        Pairs wide_at; // {byte, wide}
        Pairs byte_at; // {wide, byte}
    };
    const Case cases[] = {
        { "", { { 0, 0 } }, { { 0, 0 } } },
        { "Hello", { { 0, 0 }, { 1, 1 }, { 2, 2 }, { 3, 3 }, { 4, 4 }, { 5, 5 }, { 100, 5 } }, { { 0, 0 }, { 1, 1 }, { 2, 2 }, { 3, 3 }, { 4, 4 }, { 5, 5 }, { 100, 5 } } },
        // "日" は 3 bytes / 1 wide
        { "ab日c", { { 1, 1 }, { 2, 2 }, { 5, 3 }, { 6, 4 } }, { { 3, 5 } } },
        // ByteAt は continuation byte を返さず、次の文字の先頭 byte を返す。
        // 回帰 #183: 末尾の文字を選ぶと wide=2 が渡り、旧実装は continuation byte を返して貼り付け先で化けていた。
        { "テスト", { { 0, 0 }, { 3, 1 }, { 6, 2 }, { 9, 3 }, { 100, 3 } }, { { 0, 0 }, { 1, 3 }, { 2, 6 }, { 3, 9 }, { 100, 9 } } },
        // é = U+00E9 は 2 bytes / 1 wide
        { "aébc", { { 0, 0 }, { 1, 1 }, { 3, 2 }, { 4, 3 }, { 5, 4 } }, { { 0, 0 }, { 1, 1 }, { 2, 3 }, { 3, 4 }, { 4, 5 } } },
        // 😀 = U+1F600 は 4 bytes / 2 wide。サロゲートペアの中央 (wide=2) は絵文字ではなく次の文字の先頭 byte。
        { "A\xF0\x9F\x98\x80""B", { { 0, 0 }, { 1, 1 }, { 5, 3 }, { 6, 4 } }, { { 0, 0 }, { 1, 1 }, { 2, 5 }, { 3, 5 }, { 4, 6 } } },
    };
    for (const auto& c : cases) {
        SCOPED_TRACE(c.text);
        for (const auto [byte, wide] : c.wide_at) {
            EXPECT_EQ(WideAt(c.text, byte), wide) << "byte=" << byte;
        }
        for (const auto [wide, byte] : c.byte_at) {
            EXPECT_EQ(ByteAt(c.text, wide), byte) << "wide=" << wide;
        }
    }

    // [3, 6) byte = 「ス」 = wide [1, 2)
    const auto r = Utf16OffsetCursor{ "テスト" }.WideRange(3, 3);
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
    uint32_t face_id = 0;
    UINT16 index = 0;
    float x = 0.0f;
    float y = 0.0f;
    float size = 0.0f;
};

class GlyphCollector final : public UncountedCom<IDWriteTextRenderer> {
public:
    std::vector<DrawnGlyph> glyphs;
    std::vector<std::string> faces;

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
        const auto face_id = static_cast<uint32_t>(std::ranges::find(faces, face) - faces.begin());
        if (face_id == faces.size()) {
            faces.push_back(std::move(face));
        }
        // 右から左のランは x が右端で、ペンは左へ進む。advanceOffset も進む向きに掛かる。
        const bool rtl = (run->bidiLevel & 1) != 0;
        float pen = x;
        for (UINT32 i = 0; i < run->glyphCount; ++i) {
            const float advance = run->glyphAdvances[i];
            const DWRITE_GLYPH_OFFSET offset = run->glyphOffsets ? run->glyphOffsets[i] : DWRITE_GLYPH_OFFSET{};
            const float origin = rtl ? pen - advance - offset.advanceOffset : pen + offset.advanceOffset;
            glyphs.push_back({ face_id, run->glyphIndices[i], origin, y - offset.ascenderOffset, run->fontEmSize });
            pen += rtl ? -advance : advance;
        }
        return S_OK;
    }
};

struct Style {
    DWRITE_FONT_WEIGHT weight = DWRITE_FONT_WEIGHT_NORMAL;
    bool italic = false;
};

std::string Quote(std::wstring_view text)
{
    return testing::PrintToString(std::wstring(text));
}

// [0, count) の組を worker と分けて調べる。gtest のアサーションは worker から呼ばず、fn(i) が返す失敗の文言を
// 組ごとの枠に受けて、最後にメインスレッドで組の順に出す。
template <class Fn>
void ExpectAllInParallel(size_t count, Fn&& fn)
{
    std::vector<std::vector<std::string>> failures(count);
    {
        // アプリの計測と同じ本数。16 本にしても 1 割ほどしか縮まない (短いレイアウトは DirectWrite の内部で競合する)。
        TaskScheduler scheduler{ LayoutWorkerCount(std::thread::hardware_concurrency()) };
        ParallelFor(&scheduler, count, 1, [&](size_t begin, size_t end) {
            for (size_t i = begin; i < end; ++i) {
                try {
                    failures[i] = fn(i);
                } catch (...) {
                    failures[i] = { std::format("item {} threw", i) };
                }
            }
        });
    }
    for (const auto& item : failures) {
        for (const auto& message : item) {
            ADD_FAILURE() << message;
        }
    }
}

class FallbackFontMapTest : public ::testing::Test {
protected:
    void SetUp() override
    {
        factory_ = SharedDWriteFactory();
        ASSERT_TRUE(factory_);
    }

    ComPtr<IDWriteTextLayout> Build(std::wstring_view text, IDWriteTextFormat* format, const Style& style, const FallbackFontMap* fonts) const
    {
        ComPtr<IDWriteTextLayout> layout;
        Factory()->CreateTextLayout(text.data(), static_cast<UINT32>(text.size()), format, 300.0f, 1.0e7f, &layout);
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
        // 明示する文字が無ければ ApplyFallbackFonts は何も掛けないので、比べるまでもなく同じ。
        if (std::ranges::none_of(text, [&](wchar_t c) { return fonts.FamilyFor(c) != nullptr; })) {
            return ::testing::AssertionSuccess();
        }
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
            if (a.faces[x.face_id] != b.faces[y.face_id] || x.index != y.index || x.size != y.size || std::abs(x.x - y.x) > 0.01f || std::abs(x.y - y.y) > 0.01f) {
                return ::testing::AssertionFailure() << "glyph " << i << " differs (index " << x.index << " / " << y.index << ", x " << x.x << " / " << y.x << ")";
            }
        }
        return ::testing::AssertionSuccess();
    }

    ComPtr<IDWriteTextFormat> Format(const wchar_t* family, const wchar_t* locale, float line_height_factor = BODY_LINE_HEIGHT_FACTOR) const
    {
        return CreateTextFormat(Factory(), { .family = family, .size = 16.0f, .locale = locale, .line_height_factor = line_height_factor });
    }

    std::unique_ptr<FallbackFontMap> Map(const wchar_t* family, const wchar_t* locale) const
    {
        auto map = std::make_unique<FallbackFontMap>();
        map->Reset(Factory(), family, locale);
        return map;
    }

    // 書式を作ったスレッドは共有ファクトリ、worker は専用の ISOLATED ファクトリで組む (DWriteTextMeasurer と同じ)。
    // 共有ファクトリを全スレッドで使うと内部の競合で 4 本を超えると遅くなり、test.md の比較が 8 本で 4 本より遅い。
    IDWriteFactory* Factory() const
    {
        if (std::this_thread::get_id() == owner_) {
            return factory_.Get();
        }
        thread_local ComPtr<IDWriteFactory> isolated;
        if (!isolated) {
            DWriteCreateFactory(DWRITE_FACTORY_TYPE_ISOLATED, __uuidof(IDWriteFactory), reinterpret_cast<IUnknown**>(isolated.GetAddressOf()));
        }
        return isolated ? isolated.Get() : factory_.Get();
    }

    ComPtr<IDWriteFactory> factory_;
    std::thread::id owner_ = std::this_thread::get_id();
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

// 区間の数には上限 (kMaxFallbackRanges = 1024) があり、上限を超えた分はフォールバックに任せる。字形はどちらでも同じ。
// 漢字 1 字ずつが別の区間になるので、1100 字で上限を超える。
TEST_F(FallbackFontMapTest, GlyphsMatchFallbackBeyondRangeLimit)
{
    std::wstring text;
    for (int i = 0; i < 1100; ++i) {
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
    const auto pairs = std::views::cartesian_product(kBases, kLocales);
    ExpectAllInParallel(pairs.size(), [&](size_t i) {
        const auto [base, locale] = pairs[i];
        const auto format = Format(base, locale);
        const auto map = Map(base, locale);
        std::vector<std::string> failures;
        for (const auto [text, style] : std::views::cartesian_product(kCorpus, kStyles)) {
            if (const auto same = SameAsFallback(text, format.Get(), *map, style); !same) {
                failures.push_back(std::format("base={} locale={} text={} weight={} italic={}: {}", Quote(base), Quote(locale), Quote(text), static_cast<int>(style.weight), style.italic, same.message()));
            }
        }
        return failures;
    });
}

// ファイル一覧と目次の項目は、本文の対応を本文と違う大きさと NO_WRAP の書式に掛ける。大きさはペインのズームの両端と等倍。
TEST_F(FallbackFontMapTest, GlyphsMatchFallbackForPaneItems)
{
    constexpr const wchar_t* kTextLocales[]{ L"ja-jp", L"zh-cn", L"zh-tw", L"zh-hk", L"ko-kr" };
    constexpr std::array kZooms{ ZOOM_STEPS[0], 1.0f, ZOOM_STEPS[ZOOM_STEP_COUNT - 1] };
    const float pane_size = GetLightTheme().pane_font_size;
    const auto pairs = std::views::cartesian_product(kZooms, kTextLocales);
    ExpectAllInParallel(pairs.size(), [&](size_t i) {
        const auto [zoom, locale] = pairs[i];
        const float size = pane_size * zoom;
        const auto format = CreateTextFormat(Factory(), { .family = L"Segoe UI", .size = size, .locale = locale, .para_align = DWRITE_PARAGRAPH_ALIGNMENT_CENTER, .no_wrap = true });
        const auto map = Map(L"Segoe UI", locale);
        std::vector<std::string> failures;
        for (const wchar_t* const text : kCorpus) {
            if (const auto same = SameAsFallback(text, format.Get(), *map); !same) {
                failures.push_back(std::format("size={} locale={} text={}: {}", size, Quote(locale), Quote(text), same.message()));
            }
        }
        return failures;
    });
}

TEST_F(FallbackFontMapTest, GlyphsMatchFallbackForRealDocument)
{
    const auto bytes = ReadExampleFileBytes("example/test.md");
    ASSERT_FALSE(bytes.empty());
    std::wstring all;
    DecodeForDWrite(bytes, all);
    std::vector<std::wstring_view> lines;
    for (const auto line : all | std::views::split(L'\n')) {
        if (!line.empty()) {
            lines.emplace_back(line.begin(), line.end());
        }
    }
    // 行ごとに独立して比べるので、同じ行は 1 度でよい。
    std::ranges::sort(lines);
    const auto [dup_begin, dup_end] = std::ranges::unique(lines);
    lines.erase(dup_begin, dup_end);
    // 主フォントはアプリが実際に使う、本文の書体と ThemeService が選んだ等幅。
    // 他の等幅の候補は GlyphsMatchFallbackForEveryLanguage の文例で見る。全候補だと 286ms かかる。
    ThemeService themes;
    themes.Init(false, {});
    const std::array<const wchar_t*, 2> bases{ L"Segoe UI", themes.GetTheme().monospace_font.c_str() };
    // 対応は worker から同時に引ける。行の塊ごとに解決し直さないよう、主フォントごとに 1 つを共有する。
    std::vector<std::unique_ptr<FallbackFontMap>> maps;
    for (const auto* base : bases) {
        maps.push_back(Map(base, L"ja-jp"));
    }
    constexpr size_t kLinesPerItem = 64;
    const size_t items_per_base = (lines.size() + kLinesPerItem - 1) / kLinesPerItem;
    ExpectAllInParallel(bases.size() * items_per_base, [&](size_t i) {
        const size_t b = i / items_per_base;
        const auto format = Format(bases[b], L"ja-jp");
        for (const auto text : lines | std::views::drop(i % items_per_base * kLinesPerItem) | std::views::take(kLinesPerItem)) {
            if (const auto same = SameAsFallback(text, format.Get(), *maps[b]); !same) {
                return std::vector{ std::format("base={} text={}: {}", Quote(bases[b]), Quote(text), same.message()) };
            }
        }
        return std::vector<std::string>{};
    });
}

// 対応表の全文字を 1 文字ずつ比べる。時間がかかるので、対応表を変えたときに --gtest_also_run_disabled_tests で回す。
// どのフォントにも字形が無い文字 (未割り当てなど) は豆腐の形しか変わらないので比べない。
// 英字に挟まれた文字と、和文に挟まれた文字の両方を見る。前後の文字でフォールバック先が変わることがある。
TEST_F(FallbackFontMapTest, DISABLED_GlyphsMatchFallbackForEveryCharacter)
{
    constexpr std::pair<wchar_t, wchar_t> kNeighbours[]{ { L'a', L'b' }, { L'漢', L'字' } };
    const auto pairs = std::views::cartesian_product(kBases, kLocales);
    std::vector<size_t> mismatches(pairs.size());
    ExpectAllInParallel(pairs.size(), [&](size_t i) {
        const auto [base, locale] = pairs[i];
        const auto format = Format(base, locale, 0.0f);
        const auto map = Map(base, locale);
        std::vector<std::string> failures;
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
                    if (++mismatches[i] <= 40) {
                        failures.push_back(std::format("U+{:x} {} {} weight={} italic={}", c, Quote(base), Quote(locale), static_cast<int>(style.weight), style.italic));
                    }
                }
            }
        }
        return failures;
    });
    for (size_t i = 0; i < pairs.size(); ++i) {
        const auto [base, locale] = pairs[i];
        std::wcout << base << L" " << locale << L": " << mismatches[i] << L" mismatches\n";
    }
}
