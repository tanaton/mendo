#include "diagram_raster.h"
#include "doc_dwrite_bridge.h"
#include "ascii_util.h"
#include "string_convert.h"
#include "theme_palette.h"
#include "ui_constants.h"
#include "utf8_codec.h"
#include "utility.h"
#include "wic_util.h"
#include <d2d1_3.h>
#include <dwrite_3.h>
#include <algorithm>
#include <array>
#include <bit>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <deque>
#include <functional>
#include <limits>
#include <mutex>
#include <numbers>
#include <numeric>
#include <ranges>
#include <span>
#include <string>
#include <unordered_map>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

using namespace std::literals;
using Microsoft::WRL::ComPtr;

namespace {

constexpr std::wstring_view kDefaultFamily = L"Segoe UI"sv;
constexpr std::string_view kDefaultFamilyUtf8 = "Segoe UI"sv;
static_assert(std::ranges::equal(kDefaultFamily, kDefaultFamilyUtf8));

// 総称は本文 (Theme) と同じ書体に読み替える。等幅は Theme と同じく候補から入っているものを選ぶ。
constexpr std::pair<std::wstring_view, std::wstring_view> kGenericFamilies[]{
    { L"sans-serif"sv, kDefaultFamily       },
    { L"system-ui"sv,  kDefaultFamily       },
    { L"serif"sv,      L"Times New Roman"sv },
};

bool HasFamily(IDWriteFontCollection* fonts, const wchar_t* name) noexcept
{
    UINT32 index = 0;
    BOOL exists = FALSE;
    return fonts && SUCCEEDED(fonts->FindFamilyName(name, &index, &exists)) && exists;
}

template <class Char>
std::basic_string_view<Char> Trim(std::basic_string_view<Char> s) noexcept
{
    static constexpr Char kSpace[]{ Char(' '), Char('\t'), Char('\r'), Char('\n') };
    constexpr std::basic_string_view<Char> spaces(kSpace, std::size(kSpace));
    const auto first = s.find_first_not_of(spaces);
    if (first == s.npos) {
        return {};
    }
    return s.substr(first, s.find_last_not_of(spaces) - first + 1);
}

// 両端の同じ引用符を外す。
template <class Char>
std::basic_string_view<Char> Unquote(std::basic_string_view<Char> s) noexcept
{
    if (s.size() >= 2 && (s.front() == Char('"') || s.front() == Char('\'')) && s.back() == s.front()) {
        return s.substr(1, s.size() - 2);
    }
    return s;
}

// font-family の並びごとの、先頭の書体とフォールバック。書式ごとに組み直すと、DirectWrite がフォールバック先を
// 引いた結果も書式ごとに冷えるので、プロセスで共有する。
struct FamilyChain {
    std::wstring primary;
    ComPtr<IDWriteFontFallback> fallback; // 2 番目以降の候補 → システム。候補が 1 つなら null
};

FamilyChain ChainFor(IDWriteFactory* dw, std::wstring_view css_family)
{
    static std::mutex mutex;
    // 終了時に DirectWrite のオブジェクトを解放させない (解放の順が決まらない)。
    static auto* const chains = new std::unordered_map<std::wstring, FamilyChain>();
    const std::lock_guard lock(mutex);
    std::wstring key{ css_family };
    if (const auto it = chains->find(key); it != chains->end()) {
        return it->second;
    }
    ComPtr<IDWriteFontCollection> fonts;
    dw->GetSystemFontCollection(&fonts);
    const auto families = ResolveFontFamilies(fonts.Get(), css_family);
    FamilyChain chain{ .primary = families.empty() ? std::wstring{ kDefaultFamily } : families.front() };
    if (families.size() > 1) {
        auto rest = families | std::views::drop(1) | std::views::transform([](const std::wstring& f) static noexcept { return f.c_str(); }) |
                    std::ranges::to<std::vector>();
        constexpr DWRITE_UNICODE_RANGE kAll{ 0, 0x10FFFF };
        ComPtr<IDWriteFactory2> dw2;
        ComPtr<IDWriteFontFallbackBuilder> builder;
        ComPtr<IDWriteFontFallback> system;
        if (SUCCEEDED(dw->QueryInterface(IID_PPV_ARGS(&dw2))) && SUCCEEDED(dw2->CreateFontFallbackBuilder(&builder)) &&
            SUCCEEDED(dw2->GetSystemFontFallback(&system)) &&
            SUCCEEDED(builder->AddMapping(&kAll, 1, rest.data(), static_cast<UINT32>(rest.size()))) && SUCCEEDED(builder->AddMappings(system.Get()))) {
            builder->CreateFontFallback(&chain.fallback);
        }
    }
    return chains->emplace(std::move(key), std::move(chain)).first->second;
}

// 候補の最初の書体で組み、2 番目以降をシステムのフォールバックより先に使わせる。ブラウザが font-family の並びを
// 文字ごとに試すのと同じにする (Segoe UI だけにすると、日本語のフォールバック先がブラウザと分かれて最大 86px ずれた)。
ComPtr<IDWriteTextFormat> CreateDiagramFormat(IDWriteFactory* dw, const DiagramFont& font, const wchar_t* locale)
{
    const FamilyChain chain = ChainFor(dw, font.family);
    ComPtr<IDWriteTextFormat> format;
    if (FAILED(dw->CreateTextFormat(chain.primary.c_str(), nullptr, static_cast<DWRITE_FONT_WEIGHT>(font.weight),
                                    font.italic ? DWRITE_FONT_STYLE_ITALIC : DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, font.size,
                                    locale, &format))) {
        return nullptr;
    }
    format->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    ComPtr<IDWriteTextFormat1> format1;
    if (chain.fallback && SUCCEEDED(format.As(&format1))) {
        format1->SetFontFallback(chain.fallback.Get());
    }
    return format;
}

D2D1::Matrix3x2F ToD2D(const std::array<float, 6>& m) noexcept
{
    return D2D1::Matrix3x2F(m[0], m[1], m[2], m[3], m[4], m[5]);
}

// at の変換を base に重ね、筆を at の色にする。
void Place(ID2D1DeviceContext* dc, ID2D1SolidColorBrush* brush, const D2D1::Matrix3x2F& base, const mermaid_util::DrawingPlacement& at)
{
    dc->SetTransform(ToD2D(at.transform) * base);
    brush->SetColor(D2D1::ColorF(at.rgb, at.alpha));
}

void DrawRun(ID2D1DeviceContext* dc, IDWriteFactory* dw, DiagramTextMeasurer& formats, ID2D1SolidColorBrush* brush, const D2D1::Matrix3x2F& base,
             const mermaid_util::DrawingText& t)
{
    if (t.text.empty()) {
        return;
    }
    IDWriteTextFormat* format = formats.Format({ .family = t.family, .size = t.at.font_size, .weight = t.weight, .italic = t.italic });
    ComPtr<IDWriteTextLayout> layout;
    if (!format || FAILED(dw->CreateTextLayout(t.text.data(), static_cast<UINT32>(t.text.size()), format, 1.0e6f, 1.0e6f, &layout))) {
        return;
    }
    const DWRITE_TEXT_RANGE all{ 0, static_cast<UINT32>(t.text.size()) };
    if (t.decoration & mermaid_util::kTextUnderline) {
        layout->SetUnderline(TRUE, all);
    }
    if (t.decoration & mermaid_util::kTextLineThrough) {
        layout->SetStrikethrough(TRUE, all);
    }
    // 文字の並びは 1 行ずつなので、ベースラインは最初の行のものを使う。
    DWRITE_LINE_METRICS line{};
    UINT32 lines = 0;
    layout->GetLineMetrics(&line, 1, &lines);
    if (lines == 0) {
        return;
    }
    Place(dc, brush, base, t.at);
    dc->DrawTextLayout(D2D1::Point2F(t.at.x, t.at.y - line.baseline), layout.Get(), brush, D2D1_DRAW_TEXT_OPTIONS_ENABLE_COLOR_FONT);
}

void AppendFontKey(std::wstring& key, const DiagramFont& font)
{
    const auto bits = std::bit_cast<uint32_t>(font.size);
    key.append(font.family);
    key.push_back(L'\0');
    key.push_back(static_cast<wchar_t>(bits & 0xFFFFu));
    key.push_back(static_cast<wchar_t>(bits >> 16));
    key.push_back(static_cast<wchar_t>(font.weight));
    key.push_back(font.italic ? L'i' : L'n');
}

// ---------------------------------------------------------------------------------------------------------------
// pele の SVG の解決。ブラウザなら getComputedStyle などで求まる値を自分で求める。
// pele の出力は語彙が狭い (g・図形・text・tspan・入れ子の svg・linearGradient) ので、その範囲の CSS だけを解く。
// 利用者の style (classDef など) は pele を素通りするので、色名・hsl・単位・font-size のキーワードは読む。
// 知らない要素があれば描かずに nullopt を返し、描けない図として扱わせる。

constexpr std::string_view kSpaces = " \t\r\n"sv;

// ダークの配色。SVG のルートの style の先頭に足す。
constexpr std::string_view kDarkVars =
    "--pele-bg:#1e1e1e;--pele-fg:#e0e0e0;--pele-muted:#a0a0a0;--pele-line:#d0d0d0;"
    "--pele-surface:#2b2b2b;--pele-surface-alt:#363636;--pele-border:#7a7a7a;--pele-accent:#7c9be6;"
    "--pele-series-1:#799abe;--pele-series-2:#f8a452;--pele-series-3:#7fb978;--pele-series-4:#eb8180;"
    "--pele-series-5:#95c9c5;--pele-series-6:#f2d76c;--pele-series-7:#c59bb9;--pele-series-8:#b69886;"sv;

// ライトの配色は mermaid の既定のテーマ (theme-default) に寄せる。
// 文字は pele のまま (mermaid のノードの文字 #131300 に近い)。surface-alt は格子や済んだ作業にも使うので pele の灰のまま。
// 系列の色も pele のまま。mermaid の円グラフの淡い色は黒い縁取りが前提で、縁取りの無い pele では白地に埋もれる。
constexpr std::string_view kLightVars = "--pele-line:#333;--pele-surface:#ececff;--pele-border:#9370db;--pele-accent:#8a90dd;"sv;

// pele では 1 つの役割の色を、mermaid は部分ごとに変える。--pele-* はルートで --_* に解かれるので (CSS の計算値)、
// 差し替えるのは --_* の方。どちらも pele の内部の名前なので、pele を更新したらそろっているか確かめる。
struct ScopedVars {
    std::string_view diagram; // ルートの class。空ならどの図でも
    std::string_view cls;
    std::string_view vars;
};
// pele は枠を不透明度 0.5 で塗るので、#ffffbd で mermaid の #ffffde になる。
constexpr std::string_view kMermaidCluster = "--_a:#ffffbd;--_b:#aaaa33;--_m:#333;"sv;
constexpr std::string_view kMermaidNote = "--_a:#fff5ad;--_b:#aaaa33;"sv;
constexpr ScopedVars kLightScopedVars[]{
    // 枠を黄色にするのは flowchart と block だけ (mermaid でも class の namespace や state の入れ子は黄色でない)。
    { "pele-flowchart"sv, "pele-cluster"sv,     kMermaidCluster                            },
    { "pele-block"sv,     "pele-cluster"sv,     kMermaidCluster                            },
    // sequence の pele-note は pele-notes の中にあるが、class と state の pele-note は外にある。
    { {},                 "pele-note"sv,        kMermaidNote                               },
    { {},                 "pele-notes"sv,       kMermaidNote                               },
    { {},                 "pele-edge-label"sv,  "--_bg:rgba(232,232,232,0.8);--_m:#333;"sv },
    { {},                 "pele-edge-labels"sv, "--_m:#333;"sv                             },
    { {},                 "pele-activations"sv, "--_s:#f4f4f4;--_b:#666;"sv                },
};

// class 属性の語を順に f に渡す。f が true を返したらやめて true を返す。
template <class F>
bool AnyClass(std::string_view classes, F&& f)
{
    while (!classes.empty()) {
        const size_t sp = classes.find(' ');
        if (f(classes.substr(0, sp))) {
            return true;
        }
        if (sp == std::string_view::npos) {
            break;
        }
        classes.remove_prefix(sp + 1);
    }
    return false;
}

bool HasClass(std::string_view classes, std::string_view name) noexcept
{
    return AnyClass(classes, [name](std::string_view c) { return c == name; });
}

// 表の名前のように、どちらも実行時に決まる文字列の比較。片方が小文字のリテラルなら ascii_util::iequal。
bool IEquals(std::string_view a, std::string_view b) noexcept
{
    return a.size() == b.size() && std::ranges::equal(a, b, {}, ascii_util::ToLowerAscii, ascii_util::ToLowerAscii);
}

bool IsSpace(char c) noexcept
{
    return kSpaces.contains(c);
}

void SkipSeparators(std::string_view& s) noexcept
{
    while (!s.empty() && (IsSpace(s[0]) || s[0] == ',')) {
        s.remove_prefix(1);
    }
}

// 先頭の数を読み、s をその後ろへ進める。
bool TakeNumber(std::string_view& s, double& out) noexcept
{
    const size_t skip = !s.empty() && s[0] == '+' ? 1 : 0;
    const auto [ptr, ec] = std::from_chars(s.data() + skip, s.data() + s.size(), out);
    if (ec != std::errc{} || !std::isfinite(out)) {
        return false;
    }
    s.remove_prefix(static_cast<size_t>(ptr - s.data()));
    return true;
}

// 数か百分率。百分率は percent_scale を 100% とする。
bool ParseNumberOrPercent(std::string_view t, float percent_scale, float& out) noexcept
{
    double v = 0.0;
    if (!TakeNumber(t, v)) {
        return false;
    }
    if (t == "%") {
        v = v / 100.0 * percent_scale;
    }
    else if (!t.empty()) {
        return false;
    }
    const auto f = static_cast<float>(v);
    if (!std::isfinite(f)) {
        return false;
    }
    out = f;
    return true;
}

// 空白とカンマで区切った数を順に f に渡す。読めない字句があるか f が false を返したら false。
template <class F>
bool ForEachNumber(std::string_view s, F&& f)
{
    for (;;) {
        SkipSeparators(s);
        if (s.empty()) {
            return true;
        }
        double v = 0.0;
        if (!TakeNumber(s, v) || !f(v)) {
            return false;
        }
    }
}

// 空白とカンマで区切った数の並びを out に読み、数を返す。out に収まらないか読めなければ nullopt。
std::optional<size_t> ParseNumbers(std::string_view s, std::span<double> out)
{
    size_t n = 0;
    const bool ok = ForEachNumber(s, [&](double v) {
        if (n >= out.size()) {
            return false;
        }
        out[n++] = v;
        return true;
    });
    return ok ? std::optional{ n } : std::nullopt;
}

// separator の位置。括弧と引用符の中は見ない。無ければ npos。
size_t FindTopLevel(std::string_view s, char separator) noexcept
{
    int depth = 0;
    char quote = 0;
    for (size_t i = 0; i < s.size(); ++i) {
        const char c = s[i];
        if (quote) {
            quote = c == quote ? 0 : quote;
        }
        else if (c == '"' || c == '\'') {
            quote = c;
        }
        else if (c == '(') {
            ++depth;
        }
        else if (c == ')') {
            // 対応の無い ) は入れ子に数えない (CSS と同じく、その宣言だけが無効になる)。
            depth = std::max(depth - 1, 0);
        }
        else if (c == separator && depth == 0) {
            return i;
        }
    }
    return std::string_view::npos;
}

// separator で分けた部分を順に f に渡す (空でも 1 つは渡す)。f が false を返したらやめて false を返す。
template <class F>
bool ForEachTopLevel(std::string_view s, char separator, F&& f)
{
    for (;;) {
        const size_t end = FindTopLevel(s, separator);
        if (!f(s.substr(0, end))) {
            return false;
        }
        if (end == std::string_view::npos) {
            return true;
        }
        s.remove_prefix(end + 1);
    }
}

// style 属性の宣言 (name: value) を順に f(name, value) へ渡す。コロンの無い宣言は飛ばす。
template <class F>
bool ForEachDeclaration(std::string_view style, F&& f)
{
    return ForEachTopLevel(style, ';', [&](std::string_view decl) {
        const size_t colon = decl.find(':');
        return colon == std::string_view::npos || f(Trim(decl.substr(0, colon)), Trim(decl.substr(colon + 1)));
    });
}

// 長さの単位。per_em は font-size に掛ける。
struct LengthUnit {
    std::string_view name;
    double scale;
    bool per_em = false;
};
constexpr LengthUnit kLengthUnits[]{
    { "px", 1.0 },
    { "em", 1.0, true },
    { "ex", 0.5, true },
    { "pt", 96.0 / 72.0 },
    { "pc", 16.0 },
    { "in", 96.0 },
    { "cm", 96.0 / 2.54 },
    { "mm", 96.0 / 25.4 },
};

// 長さ。単位なしと px は CSS px。% は相手が要るので読めない扱い。pele は小さな箱の角丸に min() を使い、
// px 以外の線の太さの縁取りを calc(<太さ> + 4px) と書く。calc() は足し算と引き算だけを読む。
std::optional<float> ParseLength(std::string_view v, float font_size)
{
    v = Trim(v);
    if (ascii_util::istarts_with(v, "calc(") && v.ends_with(')')) {
        // 括弧の外の空白で切ると項と演算子が交互に並ぶ (CSS は + と - の前後に空白が要るので、符号や 1e-3 の - は切らない)。
        float total = 0.0f;
        float sign = 1.0f;
        bool want_term = true;
        const bool ok = ForEachTopLevel(v.substr(5, v.size() - 6), ' ', [&](std::string_view part) {
            part = Trim(part);
            if (part.empty()) {
                return true;
            }
            if (!want_term) {
                sign = part == "-" ? -1.0f : 1.0f;
                want_term = true;
                return part == "+" || part == "-";
            }
            const auto term = ParseLength(part, font_size);
            total += sign * term.value_or(0.0f);
            want_term = false;
            return term.has_value();
        });
        return ok && !want_term && std::isfinite(total) ? std::optional{ total } : std::nullopt;
    }
    const bool is_min = ascii_util::istarts_with(v, "min(");
    if ((is_min || ascii_util::istarts_with(v, "max(")) && v.ends_with(')')) {
        std::optional<float> out;
        const bool ok = ForEachTopLevel(v.substr(4, v.size() - 5), ',', [&](std::string_view arg) {
            const auto x = ParseLength(arg, font_size);
            if (x) {
                out = !out ? *x : is_min ? std::min(*out, *x)
                                         : std::max(*out, *x);
            }
            return x.has_value();
        });
        return ok ? out : std::nullopt;
    }
    double n = 0.0;
    if (!TakeNumber(v, n)) {
        return std::nullopt;
    }
    double scale = 1.0;
    if (!v.empty()) {
        const auto unit = std::ranges::find_if(kLengthUnits, [v](const LengthUnit& u) { return IEquals(v, u.name); });
        if (unit == std::end(kLengthUnits)) {
            return std::nullopt;
        }
        scale = unit->per_em ? unit->scale * font_size : unit->scale;
    }
    const auto out = static_cast<float>(n * scale);
    return std::isfinite(out) ? std::optional{ out } : std::nullopt;
}

// 長さか、percent_base を 100% とする百分率。
std::optional<float> ParseLengthOrPercent(std::string_view v, float font_size, float percent_base)
{
    v = Trim(v);
    if (!v.ends_with('%')) {
        return ParseLength(v, font_size);
    }
    float out = 0.0f;
    return ParseNumberOrPercent(v, percent_base, out) ? std::optional{ out } : std::nullopt;
}

// SVG の matrix(a b c d e f)。点 (x, y) を (a x + c y + e, b x + d y + f) に移す。DOMMatrix と同じく double で合成する。
struct Mat {
    double a = 1.0, b = 0.0, c = 0.0, d = 1.0, e = 0.0, f = 0.0;

    std::pair<double, double> Apply(double x, double y) const noexcept
    {
        return { a * x + c * y + e, b * x + d * y + f };
    }
    std::array<double, 6> Values() const noexcept
    {
        return { a, b, c, d, e, f };
    }
};

std::array<float, 6> ToArray(const Mat& m) noexcept
{
    return {
        static_cast<float>(m.a),
        static_cast<float>(m.b),
        static_cast<float>(m.c),
        static_cast<float>(m.d),
        static_cast<float>(m.e),
        static_cast<float>(m.f),
    };
}

// m の後に n を先に当てる (m ∘ n)。子の変換は Compose(親, 子)。
Mat Compose(const Mat& m, const Mat& n) noexcept
{
    return { m.a * n.a + m.c * n.b, m.b * n.a + m.d * n.b, m.a * n.c + m.c * n.d,
             m.b * n.c + m.d * n.d, m.a * n.e + m.c * n.f + m.e, m.b * n.e + m.d * n.f + m.f };
}

bool ParseTransform(std::string_view s, Mat& out)
{
    constexpr double kDeg = std::numbers::pi / 180.0;
    out = {};
    std::array<double, 6> v{};
    for (;;) {
        SkipSeparators(s);
        if (s.empty()) {
            return true;
        }
        const auto open = s.find('(');
        const auto close = s.find(')');
        const auto count = open == std::string_view::npos || close == std::string_view::npos || close < open
                               ? std::nullopt
                               : ParseNumbers(s.substr(open + 1, close - open - 1), v);
        if (!count) {
            return false;
        }
        const size_t n = *count;
        const auto name = Trim(s.substr(0, open));
        s.remove_prefix(close + 1);
        Mat m;
        if (name == "matrix" && n == 6) {
            m = { v[0], v[1], v[2], v[3], v[4], v[5] };
        }
        else if (name == "translate" && (n == 1 || n == 2)) {
            m.e = v[0];
            m.f = n == 2 ? v[1] : 0.0;
        }
        else if (name == "scale" && (n == 1 || n == 2)) {
            m.a = v[0];
            m.d = n == 2 ? v[1] : v[0];
        }
        else if (name == "rotate" && (n == 1 || n == 3)) {
            const double cs = std::cos(v[0] * kDeg);
            const double sn = std::sin(v[0] * kDeg);
            m = { cs, sn, -sn, cs, 0.0, 0.0 };
            if (n == 3) {
                m = Compose(Compose(Mat{ .e = v[1], .f = v[2] }, m), Mat{ .e = -v[1], .f = -v[2] });
            }
        }
        else if (name == "skewX" && n == 1) {
            m.c = std::tan(v[0] * kDeg);
        }
        else if (name == "skewY" && n == 1) {
            m.b = std::tan(v[0] * kDeg);
        }
        else {
            return false;
        }
        out = Compose(out, m);
    }
}

struct Bounds {
    double left = std::numeric_limits<double>::infinity();
    double top = std::numeric_limits<double>::infinity();
    double right = -std::numeric_limits<double>::infinity();
    double bottom = -std::numeric_limits<double>::infinity();

    void Add(double x, double y) noexcept
    {
        left = std::min(left, x);
        top = std::min(top, y);
        right = std::max(right, x);
        bottom = std::max(bottom, y);
    }
    bool Empty() const noexcept
    {
        return !(left <= right && top <= bottom);
    }
    Bounds Transformed(const Mat& m) const noexcept
    {
        Bounds out;
        if (Empty()) {
            return out;
        }
        for (const auto& [x, y] : { m.Apply(left, top), m.Apply(right, top), m.Apply(left, bottom), m.Apply(right, bottom) }) {
            out.Add(x, y);
        }
        return out;
    }
    Bounds Padded(double pad) const noexcept
    {
        return Empty() ? *this : Bounds{ left - pad, top - pad, right + pad, bottom + pad };
    }
    bool Overlaps(const Bounds& o) const noexcept
    {
        return left < o.right && o.left < right && top < o.bottom && o.top < bottom;
    }
};

// 楕円弧の、楕円全体の外接矩形 (SVG 仕様の F.6.5 で中心を求める)。層を分ける判断に使うので大きめでよい。
void AddArcBounds(Bounds& out, double x1, double y1, double rx, double ry, double rotation, bool large, bool sweep, double x2, double y2)
{
    out.Add(x2, y2);
    rx = std::abs(rx);
    ry = std::abs(ry);
    if (rx == 0.0 || ry == 0.0) {
        return;
    }
    const double phi = rotation * std::numbers::pi / 180.0;
    const double cs = std::cos(phi);
    const double sn = std::sin(phi);
    const double dx = (x1 - x2) / 2.0;
    const double dy = (y1 - y2) / 2.0;
    const double xp = cs * dx + sn * dy;
    const double yp = -sn * dx + cs * dy;
    const double lambda = xp * xp / (rx * rx) + yp * yp / (ry * ry);
    if (lambda > 1.0) {
        rx *= std::sqrt(lambda);
        ry *= std::sqrt(lambda);
    }
    const double num = rx * rx * ry * ry - rx * rx * yp * yp - ry * ry * xp * xp;
    const double den = rx * rx * yp * yp + ry * ry * xp * xp;
    const double coef = (den > 0.0 ? std::sqrt(std::max(0.0, num / den)) : 0.0) * (large == sweep ? -1.0 : 1.0);
    const double cxp = coef * rx * yp / ry;
    const double cyp = -coef * ry * xp / rx;
    const double cx = cs * cxp - sn * cyp + (x1 + x2) / 2.0;
    const double cy = sn * cxp + cs * cyp + (y1 + y2) / 2.0;
    const double hx = std::sqrt(rx * rx * cs * cs + ry * ry * sn * sn);
    const double hy = std::sqrt(rx * rx * sn * sn + ry * ry * cs * cs);
    out.Add(cx - hx, cy - hy);
    out.Add(cx + hx, cy + hy);
}

// path の d を、絶対座標の区間にして visitor に順に渡す。S・T の 1 つ目の制御点は鏡映で求めて渡す。
// visitor は Move(x, y)・Line(x, y)・Cubic(x1, y1, x2, y2, x, y)・Quad(x1, y1, x, y)・
// Arc(x1, y1, rx, ry, rotation, large, sweep, x2, y2)・Close() を持つ。
// 読めないところで止めて false を返す (ブラウザも誤りの手前まで描く)。
template <class Visitor>
bool WalkPath(std::string_view d, Visitor& visitor)
{
    double x = 0.0, y = 0.0, start_x = 0.0, start_y = 0.0;
    double ctrl_x = 0.0, ctrl_y = 0.0; // 直前の C/S・Q/T の制御点
    char command = 0, last = 0;
    const auto number = [&](double& v) {
        SkipSeparators(d);
        return TakeNumber(d, v);
    };
    const auto numbers = [&](double* v, int n) {
        return std::ranges::all_of(v, v + n, number);
    };
    const auto flag = [&](bool& v) {
        SkipSeparators(d);
        if (d.empty() || (d[0] != '0' && d[0] != '1')) {
            return false;
        }
        v = d[0] == '1';
        d.remove_prefix(1);
        return true;
    };
    for (;;) {
        SkipSeparators(d);
        if (d.empty()) {
            return true;
        }
        if (ascii_util::IsAsciiAlpha(d[0])) {
            command = d[0];
            d.remove_prefix(1);
        }
        else if (command == 0) {
            return false;
        }
        const bool rel = command >= 'a';
        const double ox = rel ? x : 0.0;
        const double oy = rel ? y : 0.0;
        const char upper = static_cast<char>(command & ~0x20);
        double v[6]{};
        switch (upper) {
        case 'Z':
            visitor.Close();
            x = start_x;
            y = start_y;
            command = 0;
            last = 'Z';
            continue;
        case 'M':
            if (!numbers(v, 2)) {
                return false;
            }
            x = start_x = ox + v[0];
            y = start_y = oy + v[1];
            visitor.Move(x, y);
            command = rel ? 'l' : 'L';
            last = 'M';
            continue;
        case 'L':
            if (!numbers(v, 2)) {
                return false;
            }
            x = ox + v[0];
            y = oy + v[1];
            visitor.Line(x, y);
            break;
        case 'H':
        case 'V':
            if (!numbers(v, 1)) {
                return false;
            }
            (upper == 'H' ? x : y) = (upper == 'H' ? ox : oy) + v[0];
            visitor.Line(x, y);
            break;
        case 'C':
        case 'S': {
            const bool smooth = upper == 'S';
            const int n = smooth ? 4 : 6;
            if (!numbers(v, n)) {
                return false;
            }
            const bool reflect = last == 'C' || last == 'S';
            const double c1x = !smooth ? ox + v[0] : reflect ? 2 * x - ctrl_x
                                                             : x;
            const double c1y = !smooth ? oy + v[1] : reflect ? 2 * y - ctrl_y
                                                             : y;
            ctrl_x = ox + v[n - 4];
            ctrl_y = oy + v[n - 3];
            x = ox + v[n - 2];
            y = oy + v[n - 1];
            visitor.Cubic(c1x, c1y, ctrl_x, ctrl_y, x, y);
            break;
        }
        case 'Q':
        case 'T': {
            const bool smooth = upper == 'T';
            const int n = smooth ? 2 : 4;
            if (!numbers(v, n)) {
                return false;
            }
            const bool reflect = last == 'Q' || last == 'T';
            ctrl_x = !smooth ? ox + v[0] : reflect ? 2 * x - ctrl_x
                                                   : x;
            ctrl_y = !smooth ? oy + v[1] : reflect ? 2 * y - ctrl_y
                                                   : y;
            x = ox + v[n - 2];
            y = oy + v[n - 1];
            visitor.Quad(ctrl_x, ctrl_y, x, y);
            break;
        }
        case 'A': {
            bool large = false;
            bool sweep = false;
            if (!number(v[0]) || !number(v[1]) || !number(v[2]) || !flag(large) || !flag(sweep) || !number(v[3]) || !number(v[4])) {
                return false;
            }
            const double x1 = x;
            const double y1 = y;
            x = ox + v[3];
            y = oy + v[4];
            visitor.Arc(x1, y1, v[0], v[1], v[2], large, sweep, x, y);
            break;
        }
        default:
            return false;
        }
        last = upper;
    }
}

// path の d の外接矩形 (曲線は制御点を含める)。読めないところで止め、そこまでの範囲を返す。
Bounds PathBounds(std::string_view d)
{
    struct Visitor {
        Bounds out;
        void Move(double x, double y)
        {
            out.Add(x, y);
        }
        void Line(double x, double y)
        {
            out.Add(x, y);
        }
        void Cubic(double x1, double y1, double x2, double y2, double x, double y)
        {
            out.Add(x1, y1);
            out.Add(x2, y2);
            out.Add(x, y);
        }
        void Quad(double x1, double y1, double x, double y)
        {
            out.Add(x1, y1);
            out.Add(x, y);
        }
        void Arc(double x1, double y1, double rx, double ry, double rotation, bool large, bool sweep, double x2, double y2)
        {
            AddArcBounds(out, x1, y1, rx, ry, rotation, large, sweep, x2, y2);
        }
        void Close()
        {}
    } visitor;
    WalkPath(d, visitor);
    return visitor.out;
}

// polygon・polyline の points の外接矩形。読めなければ空 (層を分ける判断に使わない)。
Bounds PointsBounds(std::string_view points)
{
    Bounds out;
    std::optional<double> x;
    const bool ok = ForEachNumber(points, [&](double v) {
        if (x) {
            out.Add(*x, v);
            x.reset();
        }
        else {
            x = v;
        }
        return true;
    });
    return ok ? out : Bounds{};
}

// ---- XML ----

constexpr uint32_t kNoNode = UINT32_MAX;

// 値と文字は、実体参照が無ければ元の SVG を指し、あれば XmlDoc::decoded を指す。
struct XmlAttr {
    std::string_view name;
    std::string_view value;
};

// name が空の節は文字。attrs と children は XmlDoc の配列を指す。
struct XmlNode {
    std::string_view name;
    std::string_view text;
    std::span<const XmlAttr> attrs;
    std::span<const uint32_t> children;
    uint32_t parent = kNoNode;

    std::optional<std::string_view> Attr(std::string_view n) const noexcept
    {
        const auto it = std::ranges::find(attrs, n, &XmlAttr::name);
        return it == attrs.end() ? std::nullopt : std::optional{ it->value };
    }
};

// 根の要素は nodes[0]。名前は元の SVG を指すので、SVG は XmlDoc より長く生かす。
// 節ごとに vector を持つと要素ごとに 2 回確保するので、属性と子は文書で 1 つの配列に詰める。節がそれを指すので写さない。
struct XmlDoc {
    std::vector<XmlNode> nodes;
    std::vector<XmlAttr> attrs;
    std::vector<uint32_t> children; // 親ごとに文書の順
    std::deque<std::string> decoded;

    XmlDoc() = default;
    XmlDoc(const XmlDoc&) = delete;
    XmlDoc& operator=(const XmlDoc&) = delete;
};

std::optional<std::string_view> DecodeXmlText(std::string_view in, XmlDoc& doc)
{
    if (!in.contains('&')) {
        return in;
    }
    std::string& out = doc.decoded.emplace_back();
    out.reserve(in.size());
    for (;;) {
        const auto amp = in.find('&');
        out.append(in.substr(0, amp));
        if (amp == std::string_view::npos) {
            return out;
        }
        const auto semi = in.find(';', amp);
        if (semi == std::string_view::npos || semi - amp > 12) {
            return std::nullopt;
        }
        const auto name = in.substr(amp + 1, semi - amp - 1);
        if (name == "amp") {
            out += '&';
        }
        else if (name == "lt") {
            out += '<';
        }
        else if (name == "gt") {
            out += '>';
        }
        else if (name == "quot") {
            out += '"';
        }
        else if (name == "apos") {
            out += '\'';
        }
        else if (name.size() > 1 && name[0] == '#') {
            const bool hex = name[1] == 'x' || name[1] == 'X';
            const auto digits = name.substr(hex ? 2 : 1);
            uint32_t cp = 0;
            char buf[4];
            const auto [ptr, ec] = std::from_chars(digits.data(), digits.data() + digits.size(), cp, hex ? 16 : 10);
            const uint32_t len = cp > 0 && cp <= 0x10FFFF ? utf8_codec::EncodeCp(cp, buf) : 0;
            if (digits.empty() || ec != std::errc{} || ptr != digits.data() + digits.size() || len == 0) {
                return std::nullopt;
            }
            out.append(buf, len);
        }
        else {
            return std::nullopt;
        }
        in.remove_prefix(semi + 1);
    }
}

// 要素・属性・文字だけの XML。
bool ParseXml(std::string_view s, XmlDoc& doc)
{
    constexpr auto npos = std::string_view::npos;
    auto& nodes = doc.nodes;
    std::vector<uint32_t> open;
    std::vector<uint32_t> attr_ends; // 節ごとの doc.attrs の終わり
    const auto add = [&](XmlNode&& node) {
        const auto index = static_cast<uint32_t>(nodes.size());
        node.parent = open.empty() ? kNoNode : open.back();
        nodes.push_back(std::move(node));
        attr_ends.push_back(static_cast<uint32_t>(doc.attrs.size()));
        return index;
    };
    const auto add_text = [&](std::string_view text) {
        if (!open.empty() && !text.empty()) {
            add(XmlNode{ .text = text });
        }
    };
    size_t i = 0;
    while (i < s.size()) {
        if (s[i] != '<') {
            const size_t end = std::min(s.find('<', i), s.size());
            const auto text = DecodeXmlText(s.substr(i, end - i), doc);
            if (!text) {
                return false;
            }
            add_text(*text);
            i = end;
            continue;
        }
        const auto rest = s.substr(i);
        const auto skip_past = [&](std::string_view terminator) {
            const size_t e = s.find(terminator, i);
            i = e == npos ? npos : e + terminator.size();
            return e != npos;
        };
        if (rest.starts_with("<!--")) {
            if (!skip_past("-->")) {
                return false;
            }
            continue;
        }
        if (rest.starts_with("<![CDATA[")) {
            const size_t e = s.find("]]>", i);
            if (e == npos) {
                return false;
            }
            add_text(s.substr(i + 9, e - i - 9));
            i = e + 3;
            continue;
        }
        if (rest.starts_with("<?") || rest.starts_with("<!")) {
            if (!skip_past(rest[1] == '?' ? "?>"sv : ">"sv)) {
                return false;
            }
            continue;
        }
        if (rest.starts_with("</")) {
            const size_t e = s.find('>', i);
            if (e == npos || open.empty() || nodes[open.back()].name != Trim(s.substr(i + 2, e - i - 2))) {
                return false;
            }
            open.pop_back();
            i = e + 1;
            continue;
        }
        size_t p = i + 1;
        const size_t name_end = s.find_first_of(" \t\r\n/>", p);
        if (name_end == npos || name_end == p) {
            return false;
        }
        XmlNode element{ .name = s.substr(p, name_end - p) };
        p = name_end;
        bool closed = false;
        for (;;) {
            p = s.find_first_not_of(kSpaces, p);
            if (p == npos) {
                return false;
            }
            if (s[p] == '>') {
                ++p;
                break;
            }
            if (s[p] == '/') {
                if (p + 1 >= s.size() || s[p + 1] != '>') {
                    return false;
                }
                p += 2;
                closed = true;
                break;
            }
            const size_t eq = s.find_first_of("= \t\r\n/>", p);
            if (eq == npos || eq == p) {
                return false;
            }
            const auto name = s.substr(p, eq - p);
            p = s.find_first_not_of(kSpaces, eq);
            if (p == npos || s[p] != '=') {
                return false;
            }
            p = s.find_first_not_of(kSpaces, p + 1);
            if (p == npos || (s[p] != '"' && s[p] != '\'')) {
                return false;
            }
            const size_t close = s.find(s[p], p + 1);
            const auto value = close == npos ? std::nullopt : DecodeXmlText(s.substr(p + 1, close - p - 1), doc);
            if (!value) {
                return false;
            }
            doc.attrs.push_back({ name, *value });
            p = close + 1;
        }
        if (open.empty() && !nodes.empty()) {
            return false;
        }
        const auto index = add(std::move(element));
        if (!closed) {
            open.push_back(index);
        }
        i = p;
    }
    if (!open.empty() || nodes.empty()) {
        return false;
    }
    // 子を親ごとにまとめる (数えて詰める)。詰め終えると starts[p] は p の子の終わり、starts[p - 1] は始まりになる。
    std::vector<uint32_t> starts(nodes.size() + 1, 0);
    for (const XmlNode& n : nodes) {
        if (n.parent != kNoNode) {
            ++starts[n.parent + 1];
        }
    }
    std::partial_sum(starts.begin(), starts.end(), starts.begin());
    doc.children.resize(starts.back());
    for (uint32_t k = 0; k < nodes.size(); ++k) {
        if (nodes[k].parent != kNoNode) {
            doc.children[starts[nodes[k].parent]++] = k;
        }
    }
    const std::span<const XmlAttr> attrs{ doc.attrs };
    const std::span<const uint32_t> children{ doc.children };
    uint32_t attr_begin = 0;
    for (uint32_t k = 0; k < nodes.size(); ++k) {
        nodes[k].attrs = attrs.subspan(attr_begin, attr_ends[k] - attr_begin);
        attr_begin = attr_ends[k];
        const uint32_t child_begin = k == 0 ? 0 : starts[k - 1];
        nodes[k].children = children.subspan(child_begin, starts[k] - child_begin);
    }
    return true;
}

// ---- 色 ----

struct Rgba {
    float r = 0.0f, g = 0.0f, b = 0.0f, a = 1.0f; // 0〜1
};

struct NamedColor {
    std::string_view name;
    uint32_t rgb;
};

// CSS Color 4 の色名。二分探索するので名前の順に並べる。
constexpr NamedColor kNamedColors[]{
    { "aliceblue",            0xf0f8ff },
    { "antiquewhite",         0xfaebd7 },
    { "aqua",                 0x00ffff },
    { "aquamarine",           0x7fffd4 },
    { "azure",                0xf0ffff },
    { "beige",                0xf5f5dc },
    { "bisque",               0xffe4c4 },
    { "black",                0x000000 },
    { "blanchedalmond",       0xffebcd },
    { "blue",                 0x0000ff },
    { "blueviolet",           0x8a2be2 },
    { "brown",                0xa52a2a },
    { "burlywood",            0xdeb887 },
    { "cadetblue",            0x5f9ea0 },
    { "chartreuse",           0x7fff00 },
    { "chocolate",            0xd2691e },
    { "coral",                0xff7f50 },
    { "cornflowerblue",       0x6495ed },
    { "cornsilk",             0xfff8dc },
    { "crimson",              0xdc143c },
    { "cyan",                 0x00ffff },
    { "darkblue",             0x00008b },
    { "darkcyan",             0x008b8b },
    { "darkgoldenrod",        0xb8860b },
    { "darkgray",             0xa9a9a9 },
    { "darkgreen",            0x006400 },
    { "darkgrey",             0xa9a9a9 },
    { "darkkhaki",            0xbdb76b },
    { "darkmagenta",          0x8b008b },
    { "darkolivegreen",       0x556b2f },
    { "darkorange",           0xff8c00 },
    { "darkorchid",           0x9932cc },
    { "darkred",              0x8b0000 },
    { "darksalmon",           0xe9967a },
    { "darkseagreen",         0x8fbc8f },
    { "darkslateblue",        0x483d8b },
    { "darkslategray",        0x2f4f4f },
    { "darkslategrey",        0x2f4f4f },
    { "darkturquoise",        0x00ced1 },
    { "darkviolet",           0x9400d3 },
    { "deeppink",             0xff1493 },
    { "deepskyblue",          0x00bfff },
    { "dimgray",              0x696969 },
    { "dimgrey",              0x696969 },
    { "dodgerblue",           0x1e90ff },
    { "firebrick",            0xb22222 },
    { "floralwhite",          0xfffaf0 },
    { "forestgreen",          0x228b22 },
    { "fuchsia",              0xff00ff },
    { "gainsboro",            0xdcdcdc },
    { "ghostwhite",           0xf8f8ff },
    { "gold",                 0xffd700 },
    { "goldenrod",            0xdaa520 },
    { "gray",                 0x808080 },
    { "green",                0x008000 },
    { "greenyellow",          0xadff2f },
    { "grey",                 0x808080 },
    { "honeydew",             0xf0fff0 },
    { "hotpink",              0xff69b4 },
    { "indianred",            0xcd5c5c },
    { "indigo",               0x4b0082 },
    { "ivory",                0xfffff0 },
    { "khaki",                0xf0e68c },
    { "lavender",             0xe6e6fa },
    { "lavenderblush",        0xfff0f5 },
    { "lawngreen",            0x7cfc00 },
    { "lemonchiffon",         0xfffacd },
    { "lightblue",            0xadd8e6 },
    { "lightcoral",           0xf08080 },
    { "lightcyan",            0xe0ffff },
    { "lightgoldenrodyellow", 0xfafad2 },
    { "lightgray",            0xd3d3d3 },
    { "lightgreen",           0x90ee90 },
    { "lightgrey",            0xd3d3d3 },
    { "lightpink",            0xffb6c1 },
    { "lightsalmon",          0xffa07a },
    { "lightseagreen",        0x20b2aa },
    { "lightskyblue",         0x87cefa },
    { "lightslategray",       0x778899 },
    { "lightslategrey",       0x778899 },
    { "lightsteelblue",       0xb0c4de },
    { "lightyellow",          0xffffe0 },
    { "lime",                 0x00ff00 },
    { "limegreen",            0x32cd32 },
    { "linen",                0xfaf0e6 },
    { "magenta",              0xff00ff },
    { "maroon",               0x800000 },
    { "mediumaquamarine",     0x66cdaa },
    { "mediumblue",           0x0000cd },
    { "mediumorchid",         0xba55d3 },
    { "mediumpurple",         0x9370db },
    { "mediumseagreen",       0x3cb371 },
    { "mediumslateblue",      0x7b68ee },
    { "mediumspringgreen",    0x00fa9a },
    { "mediumturquoise",      0x48d1cc },
    { "mediumvioletred",      0xc71585 },
    { "midnightblue",         0x191970 },
    { "mintcream",            0xf5fffa },
    { "mistyrose",            0xffe4e1 },
    { "moccasin",             0xffe4b5 },
    { "navajowhite",          0xffdead },
    { "navy",                 0x000080 },
    { "oldlace",              0xfdf5e6 },
    { "olive",                0x808000 },
    { "olivedrab",            0x6b8e23 },
    { "orange",               0xffa500 },
    { "orangered",            0xff4500 },
    { "orchid",               0xda70d6 },
    { "palegoldenrod",        0xeee8aa },
    { "palegreen",            0x98fb98 },
    { "paleturquoise",        0xafeeee },
    { "palevioletred",        0xdb7093 },
    { "papayawhip",           0xffefd5 },
    { "peachpuff",            0xffdab9 },
    { "peru",                 0xcd853f },
    { "pink",                 0xffc0cb },
    { "plum",                 0xdda0dd },
    { "powderblue",           0xb0e0e6 },
    { "purple",               0x800080 },
    { "rebeccapurple",        0x663399 },
    { "red",                  0xff0000 },
    { "rosybrown",            0xbc8f8f },
    { "royalblue",            0x4169e1 },
    { "saddlebrown",          0x8b4513 },
    { "salmon",               0xfa8072 },
    { "sandybrown",           0xf4a460 },
    { "seagreen",             0x2e8b57 },
    { "seashell",             0xfff5ee },
    { "sienna",               0xa0522d },
    { "silver",               0xc0c0c0 },
    { "skyblue",              0x87ceeb },
    { "slateblue",            0x6a5acd },
    { "slategray",            0x708090 },
    { "slategrey",            0x708090 },
    { "snow",                 0xfffafa },
    { "springgreen",          0x00ff7f },
    { "steelblue",            0x4682b4 },
    { "tan",                  0xd2b48c },
    { "teal",                 0x008080 },
    { "thistle",              0xd8bfd8 },
    { "tomato",               0xff6347 },
    { "turquoise",            0x40e0d0 },
    { "violet",               0xee82ee },
    { "wheat",                0xf5deb3 },
    { "white",                0xffffff },
    { "whitesmoke",           0xf5f5f5 },
    { "yellow",               0xffff00 },
    { "yellowgreen",          0x9acd32 },
};
static_assert(std::ranges::is_sorted(kNamedColors, {}, &NamedColor::name));

Rgba FromRgb(uint32_t rgb) noexcept
{
    return { ((rgb >> 16) & 0xFF) / 255.0f, ((rgb >> 8) & 0xFF) / 255.0f, (rgb & 0xFF) / 255.0f, 1.0f };
}

bool ParseHexColor(std::string_view hex, Rgba& out) noexcept
{
    uint32_t v = 0;
    const auto [ptr, ec] = std::from_chars(hex.data(), hex.data() + hex.size(), v, 16);
    if (ec != std::errc{} || ptr != hex.data() + hex.size()) {
        return false;
    }
    const auto nibble = [v](int i) { return static_cast<float>((v >> (i * 4)) & 0xF) * 17.0f / 255.0f; };
    const auto byte = [v](int i) { return static_cast<float>((v >> (i * 8)) & 0xFF) / 255.0f; };
    switch (hex.size()) {
    case 3:
        out = { nibble(2), nibble(1), nibble(0), 1.0f };
        return true;
    case 4:
        out = { nibble(3), nibble(2), nibble(1), nibble(0) };
        return true;
    case 6:
        out = { byte(2), byte(1), byte(0), 1.0f };
        return true;
    case 8:
        out = { byte(3), byte(2), byte(1), byte(0) };
        return true;
    default:
        return false;
    }
}

// 関数の引数を、空白・カンマ・/ で区切った字句にする。/ はそれ自体も字句にする (rgb(r g b / a))。
std::vector<std::string_view> FunctionTokens(std::string_view s)
{
    std::vector<std::string_view> out;
    size_t i = 0;
    while (i < s.size()) {
        if (IsSpace(s[i]) || s[i] == ',') {
            ++i;
            continue;
        }
        if (s[i] == '/') {
            out.push_back(s.substr(i, 1));
            ++i;
            continue;
        }
        const size_t end = std::min(s.find_first_of(" \t\r\n,/", i), s.size());
        out.push_back(s.substr(i, end - i));
        i = end;
    }
    return out;
}

bool ParseAlpha(std::span<const std::string_view> tokens, float& alpha) noexcept
{
    if (tokens.empty()) {
        alpha = 1.0f;
        return true;
    }
    if (tokens[0] == "/") {
        tokens = tokens.subspan(1);
    }
    if (tokens.size() != 1 || !ParseNumberOrPercent(tokens[0], 1.0f, alpha)) {
        return false;
    }
    alpha = std::clamp(alpha, 0.0f, 1.0f);
    return true;
}

bool ParseRgbFunction(std::string_view args, Rgba& out)
{
    const auto t = FunctionTokens(args);
    float c[3]{};
    if (t.size() < 3) {
        return false;
    }
    for (int i = 0; i < 3; ++i) {
        if (!ParseNumberOrPercent(t[i], 255.0f, c[i])) {
            return false;
        }
        c[i] = std::clamp(std::round(c[i]), 0.0f, 255.0f) / 255.0f;
    }
    float alpha = 1.0f;
    if (!ParseAlpha(std::span{ t }.subspan(3), alpha)) {
        return false;
    }
    out = { c[0], c[1], c[2], alpha };
    return true;
}

bool ParseHslFunction(std::string_view args, Rgba& out)
{
    const auto t = FunctionTokens(args);
    if (t.size() < 3) {
        return false;
    }
    std::string_view hue = t[0];
    double h = 0.0;
    if (!TakeNumber(hue, h)) {
        return false;
    }
    if (hue == "turn") {
        h *= 360.0;
    }
    else if (hue == "rad") {
        h *= 180.0 / std::numbers::pi;
    }
    else if (!hue.empty() && hue != "deg") {
        return false;
    }
    float s = 0.0f;
    float l = 0.0f;
    float alpha = 1.0f;
    if (!ParseNumberOrPercent(t[1], 1.0f, s) || !ParseNumberOrPercent(t[2], 1.0f, l) || !ParseAlpha(std::span{ t }.subspan(3), alpha)) {
        return false;
    }
    // 単位なしの s・l は 0〜100
    if (!t[1].ends_with('%')) {
        s /= 100.0f;
    }
    if (!t[2].ends_with('%')) {
        l /= 100.0f;
    }
    s = std::clamp(s, 0.0f, 1.0f);
    l = std::clamp(l, 0.0f, 1.0f);
    h = std::fmod(std::fmod(h, 360.0) + 360.0, 360.0);
    const auto channel = [&](double n) {
        const double k = std::fmod(n + h / 30.0, 12.0);
        const double a = s * std::min(l, 1.0f - l);
        return static_cast<float>(l - a * std::max(-1.0, std::min({ k - 3.0, 9.0 - k, 1.0 })));
    };
    const auto byte = [](float v) { return std::round(std::clamp(v, 0.0f, 1.0f) * 255.0f) / 255.0f; };
    out = { byte(channel(0)), byte(channel(8)), byte(channel(4)), alpha };
    return true;
}

bool ParseColor(std::string_view v, const Rgba& current, Rgba& out);

// color-mix(in srgb, A [p%], B [q%])。乗算済みの sRGB で混ぜる (CSS Color 5)。
bool ParseColorMix(std::string_view args, const Rgba& current, Rgba& out)
{
    std::string_view parts[3];
    size_t n = 0;
    ForEachTopLevel(args, ',', [&](std::string_view part) {
        if (n < std::size(parts)) {
            parts[n] = part;
        }
        return ++n <= std::size(parts);
    });
    if (n != std::size(parts) || !ascii_util::iequal(Trim(parts[0]), "in srgb")) {
        return false;
    }
    Rgba c[2];
    float p[2]{ -1.0f, -1.0f };
    for (int i = 0; i < 2; ++i) {
        std::string_view part = Trim(parts[i + 1]);
        // 百分率は色の前でも後ろでもよい。
        const size_t last_space = part.find_last_of(kSpaces);
        const size_t first_space = part.find_first_of(kSpaces);
        if (last_space != std::string_view::npos && part.ends_with('%') &&
            ParseNumberOrPercent(part.substr(last_space + 1), 100.0f, p[i])) {
            part = Trim(part.substr(0, last_space));
        }
        else if (first_space != std::string_view::npos && part.substr(0, first_space).ends_with('%') &&
                 ParseNumberOrPercent(part.substr(0, first_space), 100.0f, p[i])) {
            part = Trim(part.substr(first_space + 1));
        }
        if (!ParseColor(part, current, c[i])) {
            return false;
        }
    }
    if (p[0] < 0.0f && p[1] < 0.0f) {
        p[0] = p[1] = 50.0f;
    }
    else if (p[0] < 0.0f) {
        p[0] = 100.0f - p[1];
    }
    else if (p[1] < 0.0f) {
        p[1] = 100.0f - p[0];
    }
    const float sum = p[0] + p[1];
    if (!(sum > 0.0f) || p[0] < 0.0f || p[1] < 0.0f) {
        return false;
    }
    const float w0 = p[0] / sum;
    const float w1 = p[1] / sum;
    const float a = c[0].a * w0 + c[1].a * w1;
    const auto mix = [&](float Rgba::* ch) { return a > 0.0f ? (c[0].*ch * c[0].a * w0 + c[1].*ch * c[1].a * w1) / a : 0.0f; };
    out = { mix(&Rgba::r), mix(&Rgba::g), mix(&Rgba::b), a * std::min(sum, 100.0f) / 100.0f };
    return true;
}

bool ParseColor(std::string_view v, const Rgba& current, Rgba& out)
{
    v = Trim(v);
    if (v.empty()) {
        return false;
    }
    if (v[0] == '#') {
        return ParseHexColor(v.substr(1), out);
    }
    if (ascii_util::iequal(v, "currentcolor")) {
        out = current;
        return true;
    }
    if (ascii_util::iequal(v, "transparent")) {
        out = { 0.0f, 0.0f, 0.0f, 0.0f };
        return true;
    }
    const auto paren = v.find('(');
    if (paren == std::string_view::npos) {
        char buffer[32];
        if (v.size() > std::size(buffer)) {
            return false;
        }
        std::ranges::transform(v, buffer, ascii_util::ToLowerAscii);
        const std::string_view lower{ buffer, v.size() };
        const auto it = std::ranges::lower_bound(kNamedColors, lower, {}, &NamedColor::name);
        if (it == std::end(kNamedColors) || it->name != lower) {
            return false;
        }
        out = FromRgb(it->rgb);
        return true;
    }
    if (v.back() != ')') {
        return false;
    }
    const auto name = Trim(v.substr(0, paren));
    const auto args = v.substr(paren + 1, v.size() - paren - 2);
    if (ascii_util::iequal(name, "rgb") || ascii_util::iequal(name, "rgba")) {
        return ParseRgbFunction(args, out);
    }
    if (ascii_util::iequal(name, "hsl") || ascii_util::iequal(name, "hsla")) {
        return ParseHslFunction(args, out);
    }
    if (ascii_util::iequal(name, "color-mix")) {
        return ParseColorMix(args, current, out);
    }
    return false;
}

uint32_t ColorByte(float v) noexcept
{
    return static_cast<uint32_t>(std::lround(std::clamp(v, 0.0f, 1.0f) * 255.0f));
}

uint32_t ToRgb(const Rgba& c) noexcept
{
    return (ColorByte(c.r) << 16) | (ColorByte(c.g) << 8) | ColorByte(c.b);
}

// 書き出す値 (小数 3 桁)。
double RoundNum(double v) noexcept
{
    const double r = std::round(v * 1000.0) / 1000.0;
    return r == 0.0 ? 0.0 : r;
}

// 小数 3 桁に丸めて短く書く。
void AppendNum(std::string& out, double v)
{
    const double r = RoundNum(v);
    char buf[32];
    const auto [ptr, ec] = std::to_chars(buf, buf + sizeof(buf), r);
    out.append(buf, ec == std::errc{} ? ptr : buf);
}

template <class F>
void AppendAttrWith(std::string& out, std::string_view name, F&& write_value)
{
    out += ' ';
    out += name;
    out += "=\"";
    write_value();
    out += '"';
}

void AppendAttr(std::string& out, std::string_view name, double v)
{
    AppendAttrWith(out, name, [&] { AppendNum(out, v); });
}

void AppendAttr(std::string& out, std::string_view name, std::string_view v)
{
    AppendAttrWith(out, name, [&] { ascii_util::AppendHtmlEscaped(out, v); });
}

void AppendAttr(std::string& out, std::string_view name, const Rgba& c)
{
    AppendAttrWith(out, name, [&] { ascii_util::AppendHexColor(out, ToRgb(c)); });
}

// 書き出す SVG は根の <svg> の直下に要素を平らに並べ、継承する値が無いので、SVG の初期値と同じ値は書かない。
void AppendAttrUnlessInitial(std::string& out, std::string_view name, double v, double initial)
{
    if (RoundNum(v) != initial) {
        AppendAttr(out, name, v);
    }
}

// transform 属性。平行移動だけなら translate、単位行列なら書かない (初期値)。
template <class T>
void AppendTransform(std::string& out, const std::array<T, 6>& m)
{
    if (RoundNum(m[0]) == 1.0 && RoundNum(m[1]) == 0.0 && RoundNum(m[2]) == 0.0 && RoundNum(m[3]) == 1.0) {
        if (RoundNum(m[4]) == 0.0 && RoundNum(m[5]) == 0.0) {
            return;
        }
        out += " transform=\"translate(";
        AppendNum(out, m[4]);
        out += ' ';
        AppendNum(out, m[5]);
        out += ")\"";
        return;
    }
    out += " transform=\"matrix(";
    for (const T v : m) {
        AppendNum(out, v);
        out += ' ';
    }
    out.back() = ')';
    out += '"';
}

void AppendSvgHead(std::string& out, const mermaid_util::DiagramDrawing& drawing)
{
    out += "<svg xmlns=\"http://www.w3.org/2000/svg\"";
    AppendAttr(out, "width", drawing.width);
    AppendAttr(out, "height", drawing.height);
    out += " viewBox=\"0 0 ";
    AppendNum(out, drawing.width);
    out += ' ';
    AppendNum(out, drawing.height);
    out += "\">";
    if (!drawing.defs.empty()) {
        out += "<defs>";
        out += drawing.defs;
        out += "</defs>";
    }
}

// ---- スタイル ----

struct Paint {
    enum class Kind : uint8_t {
        None,
        Color,
        CurrentColor,
        Url
    };
    Kind kind = Kind::None;
    Rgba color{};
    std::string_view url;      // Url のときの id
    bool has_fallback = false; // Url の後ろの色。無ければ none
};

bool ParsePaint(std::string_view v, const Rgba& current, Paint& out)
{
    v = Trim(v);
    if (ascii_util::iequal(v, "none")) {
        out = {};
        return true;
    }
    if (ascii_util::iequal(v, "currentcolor")) {
        out = { .kind = Paint::Kind::CurrentColor };
        return true;
    }
    if (ascii_util::istarts_with(v, "url(")) {
        const auto close = v.find(')');
        if (close == std::string_view::npos) {
            return false;
        }
        const auto ref = Unquote(Trim(v.substr(4, close - 4)));
        if (ref.empty() || ref[0] != '#') {
            return false;
        }
        Paint paint{ .kind = Paint::Kind::Url, .url = ref.substr(1) };
        const auto fallback = Trim(v.substr(close + 1));
        if (!fallback.empty() && !ascii_util::iequal(fallback, "none")) {
            if (!ParseColor(fallback, current, paint.color)) {
                return false;
            }
            paint.has_fallback = true;
        }
        out = paint;
        return true;
    }
    Rgba c;
    if (!ParseColor(v, current, c)) {
        return false;
    }
    out = { .kind = Paint::Kind::Color, .color = c };
    return true;
}

bool ParseOpacityValue(std::string_view v, float& out) noexcept
{
    if (!ParseNumberOrPercent(Trim(v), 1.0f, out)) {
        return false;
    }
    out = std::clamp(out, 0.0f, 1.0f);
    return true;
}

// 並びは当てる順。color は currentColor を使う塗りより、font-size は em を使う長さより先に当てる。
enum class Prop : uint8_t {
    Color,
    FontSize,
    Fill,
    Stroke,
    FillOpacity,
    StrokeOpacity,
    StrokeWidth,
    StrokeDasharray,
    StrokeDashoffset,
    StrokeLinecap,
    StrokeLinejoin,
    StrokeMiterlimit,
    FillRule,
    Opacity,
    Display,
    Visibility,
    FontFamily,
    FontWeight,
    FontStyle,
    TextAnchor,
    TextDecoration,
    BaselineShift,
    Rx,
    Ry,
    StopColor,
    StopOpacity,
    MaxWidth,
    Count,
};
constexpr size_t kPropCount = static_cast<size_t>(Prop::Count);

constexpr std::pair<std::string_view, Prop> kPropNames[]{
    { "color",                Prop::Color            },
    { "font-size",            Prop::FontSize         },
    { "fill",                 Prop::Fill             },
    { "stroke",               Prop::Stroke           },
    { "fill-opacity",         Prop::FillOpacity      },
    { "stroke-opacity",       Prop::StrokeOpacity    },
    { "stroke-width",         Prop::StrokeWidth      },
    { "stroke-dasharray",     Prop::StrokeDasharray  },
    { "stroke-dashoffset",    Prop::StrokeDashoffset },
    { "stroke-linecap",       Prop::StrokeLinecap    },
    { "stroke-linejoin",      Prop::StrokeLinejoin   },
    { "stroke-miterlimit",    Prop::StrokeMiterlimit },
    { "fill-rule",            Prop::FillRule         },
    { "opacity",              Prop::Opacity          },
    { "display",              Prop::Display          },
    { "visibility",           Prop::Visibility       },
    { "font-family",          Prop::FontFamily       },
    { "font-weight",          Prop::FontWeight       },
    { "font-style",           Prop::FontStyle        },
    { "text-anchor",          Prop::TextAnchor       },
    { "text-decoration",      Prop::TextDecoration   },
    { "text-decoration-line", Prop::TextDecoration   },
    { "baseline-shift",       Prop::BaselineShift    },
    { "rx",                   Prop::Rx               },
    { "ry",                   Prop::Ry               },
    { "stop-color",           Prop::StopColor        },
    { "stop-opacity",         Prop::StopOpacity      },
    { "max-width",            Prop::MaxWidth         },
};

std::optional<Prop> PropOf(std::string_view name) noexcept
{
    for (const auto& [n, p] : kPropNames) {
        if (IEquals(n, name)) {
            return p;
        }
    }
    return std::nullopt;
}

// 要素で宣言されたカスタムプロパティ。値は var() を残したまま持ち、使う要素で解く。
struct Vars {
    const Vars* parent = nullptr;
    std::vector<std::pair<std::string_view, std::string_view>> values;

    const std::string_view* Find(std::string_view name) const noexcept
    {
        for (const Vars* v = this; v; v = v->parent) {
            for (auto it = v->values.rbegin(); it != v->values.rend(); ++it) {
                if (it->first == name) {
                    return &it->second;
                }
            }
        }
        return nullptr;
    }
};

size_t FindVar(std::string_view s) noexcept
{
    return ascii_util::Find(s, "var(", 0, /*fold=*/true);
}

// var() を置き換える。参照先が無く代わりの値も無ければ false (計算値の時点で無効)。
bool SubstituteVars(std::string_view in, const Vars* vars, std::string& out, int depth = 0)
{
    if (depth > 16) {
        return false;
    }
    for (;;) {
        const size_t pos = FindVar(in);
        if (pos == std::string_view::npos) {
            out.append(in);
            return true;
        }
        out.append(in.substr(0, pos));
        int level = 0;
        size_t close = std::string_view::npos;
        for (size_t i = pos + 3; i < in.size(); ++i) {
            level += in[i] == '(' ? 1 : in[i] == ')' ? -1
                                                     : 0;
            if (level == 0) {
                close = i;
                break;
            }
        }
        if (close == std::string_view::npos) {
            return false;
        }
        const auto inner = in.substr(pos + 4, close - pos - 4);
        const size_t comma = FindTopLevel(inner, ',');
        const auto name = Trim(inner.substr(0, comma));
        bool done = false;
        if (const auto* value = vars ? vars->Find(name) : nullptr) {
            const size_t mark = out.size();
            done = SubstituteVars(*value, vars, out, depth + 1);
            if (!done) {
                out.resize(mark);
            }
        }
        // 参照先が無効なら代わりの値を使う。
        if (!done && (comma == std::string_view::npos || !SubstituteVars(Trim(inner.substr(comma + 1)), vars, out, depth + 1))) {
            return false;
        }
        in.remove_prefix(close + 1);
    }
}

struct Style {
    // 継承する
    Paint fill{ .kind = Paint::Kind::Color };
    Paint stroke{};
    Rgba color{};
    float fill_opacity = 1.0f;
    float stroke_opacity = 1.0f;
    float stroke_width = 1.0f;
    float dash_offset = 0.0f;
    float miter_limit = 4.0f;
    std::string_view dash_array; // 空は none。数を空白で区切って正規化したもの
    std::string_view line_cap = "butt";
    std::string_view line_join = "miter";
    bool even_odd = false;
    bool visible = true;
    bool preserve_space = false;
    std::wstring_view font_family = L"sans-serif";
    float font_size = 16.0f;
    uint16_t weight = 400;
    bool italic = false;
    uint8_t anchor = 0; // 0 start、1 middle、2 end
    const Vars* vars = nullptr;
    // 継承しない。Compute が要素ごとに初期値へ戻す。
    struct Own {
        float opacity = 1.0f;
        bool display_none = false;
        uint8_t decoration = 0; // mermaid_util::kTextUnderline・kTextLineThrough
        bool fit_width = false; // max-width:100%
        std::string_view baseline_shift;
        std::optional<float> rx; // nullopt は auto
        std::optional<float> ry;
        Rgba stop_color{};
        float stop_opacity = 1.0f;
    } own;

    DiagramFont Font() const noexcept
    {
        return { .family = font_family, .size = font_size, .weight = weight, .italic = italic };
    }
};

// 単色の塗り。グラデーションと none は nullopt。
std::optional<Rgba> SolidColor(const Paint& paint, const Style& s) noexcept
{
    switch (paint.kind) {
    case Paint::Kind::Color:
        return paint.color;
    case Paint::Kind::CurrentColor:
        return s.color;
    default:
        return std::nullopt;
    }
}

uint16_t RelativeWeight(std::string_view v, uint16_t parent) noexcept
{
    if (ascii_util::iequal(v, "bolder")) {
        return parent < 350 ? 400 : parent < 550 ? 700
                                                 : 900;
    }
    if (ascii_util::iequal(v, "lighter")) {
        return parent < 550 ? 100 : parent < 750 ? 400
                                                 : 700;
    }
    return 0;
}

// font-size。smaller・larger は Chromium と同じく 1.2 倍 (pele の measurer の下付き・上付きも 1.2 で割る)。
std::optional<float> ParseFontSize(std::string_view v, float parent)
{
    constexpr std::pair<std::string_view, float> kKeywords[]{
        { "xx-small", 9.0f  },
        { "x-small",  10.0f },
        { "small",    13.0f },
        { "medium",   16.0f },
        { "large",    18.0f },
        { "x-large",  24.0f },
        { "xx-large", 32.0f },
    };
    v = Trim(v);
    if (ascii_util::iequal(v, "smaller")) {
        return parent / 1.2f;
    }
    if (ascii_util::iequal(v, "larger")) {
        return parent * 1.2f;
    }
    for (const auto& [name, size] : kKeywords) {
        if (IEquals(v, name)) {
            return size;
        }
    }
    const auto size = ParseLengthOrPercent(v, parent, parent);
    return size && *size >= 0.0f ? size : std::nullopt;
}

// 属性の長さ。無いか読めなければ nullopt (ブラウザは読めない値を初期値の 0 にする)。
std::optional<float> AttrLength(const XmlNode& el, std::string_view name, float font_size)
{
    const auto attr = el.Attr(name);
    return attr ? ParseLength(*attr, font_size) : std::nullopt;
}

// viewBox と preserveAspectRatio による、(x, y, width, height) の枠への変換。根と入れ子の svg に使う。
bool ViewportTransform(const XmlNode& el, double x, double y, double width, double height, Mat& out)
{
    out = { .e = x, .f = y };
    const auto view_box = el.Attr("viewBox");
    if (!view_box) {
        return true;
    }
    std::array<double, 4> vb{};
    if (ParseNumbers(*view_box, vb) != vb.size() || !(vb[2] > 0.0 && vb[3] > 0.0)) {
        return false;
    }
    double sx = width / vb[2];
    double sy = height / vb[3];
    std::string_view align = "xMidYMid";
    bool slice = false;
    if (const auto par = el.Attr("preserveAspectRatio")) {
        const auto v = Trim(*par);
        const auto space = v.find(' ');
        align = v.substr(0, space);
        slice = space != std::string_view::npos && Trim(v.substr(space)) == "slice";
    }
    double tx = 0.0;
    double ty = 0.0;
    if (align != "none") {
        sx = sy = slice ? std::max(sx, sy) : std::min(sx, sy);
        const double fx = align.contains("xMid") ? 0.5 : align.contains("xMax") ? 1.0
                                                                                : 0.0;
        const double fy = align.contains("YMid") ? 0.5 : align.contains("YMax") ? 1.0
                                                                                : 0.0;
        tx = (width - vb[2] * sx) * fx;
        ty = (height - vb[3] * sy) * fy;
    }
    out = { sx, 0.0, 0.0, sy, x + tx - vb[0] * sx, y + ty - vb[1] * sy };
    return true;
}

class PeleSvgResolver {
public:
    PeleSvgResolver(const XmlDoc& doc, DiagramTextMeasurer& measurer, std::span<const PeleMathLabel> maths)
        : nodes_(doc.nodes), measurer_(measurer), maths_(maths)
    {
    }

    std::optional<mermaid_util::DiagramDrawing> Run(bool dark, float max_width);

private:
    struct Piece {
        std::string_view raw;
        Style style;
        float opacity = 1.0f;
        uint8_t decoration = 0; // 祖先の text-decoration の線も重ねる (CSS の装飾の伝播)
        float shift = 0.0f;     // baseline-shift の合計 (下が正)
        std::optional<float> x;
        std::optional<float> y;
        std::wstring text; // 空白を畳んだ後
        float width = 0.0f;
        float left = 0.0f;
        float top = 0.0f; // 塊の y (ずらす前のベースライン)
    };

    std::string_view Keep(std::string s)
    {
        return strings_.emplace_back(std::move(s));
    }
    // Apply が持ち続ける view。var() を解いた値 (resolved_) は次の宣言で上書きされるので写す。
    std::string_view Retain(std::string_view v)
    {
        return !v.empty() && IsSubrangeOf(v, resolved_) ? Keep(std::string(v)) : v;
    }
    Style Compute(const Style& parent, const XmlNode& el, std::string_view extra_style = {});
    // CSS として読めた (正しい値だった) か。読めない値では s を書き換えない。
    bool Apply(Style& s, const Style& parent, Prop prop, std::string_view value);
    std::string_view ScopedVarsFor(const XmlNode& el) const;
    bool Walk(uint32_t index, const Style& parent, const Mat& ctm, float opacity);
    bool Shape(const XmlNode& el, const Style& s, const Mat& ctm, float opacity);
    bool Text(uint32_t index, const Style& s, const Mat& ctm, float opacity);
    bool MathLabel(const XmlNode& el, const Style& s, const Mat& ctm, float opacity);
    bool CollectText(uint32_t index, const Style& s, float opacity, uint8_t decoration, float shift, std::vector<Piece>& pieces,
                     std::optional<float>& pending_x, std::optional<float>& pending_y);
    void AppendPaint(std::string& out, std::string_view name, const Paint& paint, const Style& s, float paint_opacity);
    std::string_view GradientRef(std::string_view id, float alpha);
    float BaselineShift(const Style& s);

    // 図形を書く層。図形は今の層の文字に重なるときだけ次の層へ送り、文書の順の重なりを保つ。
    // 範囲 (bounds()) は今の層に文字があるときだけ求める (path の範囲は d を読み直すので)。
    template <class F>
    std::string& ShapesFor(F&& bounds)
    {
        if (!text_bounds_.empty()) {
            const Bounds b = bounds();
            if (std::ranges::any_of(text_bounds_, [&](const Bounds& t) { return b.Overlaps(t); })) {
                layers_.emplace_back();
                text_bounds_.clear();
            }
        }
        return layers_.back().shapes;
    }

    const std::vector<XmlNode>& nodes_;
    DiagramTextMeasurer& measurer_;
    std::span<const PeleMathLabel> maths_;
    std::deque<std::string> strings_;
    std::deque<std::wstring> wide_strings_;
    std::deque<Vars> vars_;
    std::string resolved_;                                                      // var() を解いた値。Apply が持ち続けるものは Retain で写す
    std::vector<std::pair<Prop, std::string_view>> declared_;                   // Compute の宣言の並び。要素ごとに確保しないよう使い回す
    std::unordered_map<std::string_view, uint32_t, StringTransparentHash> ids_; // GradientRef が初めて引くときに作る
    bool ids_ready_ = false;
    std::vector<std::pair<std::string, std::string>> gradients_; // (id|alpha, 名前。空は塗れない参照)
    std::string defs_;
    std::vector<mermaid_util::DrawingLayer> layers_{ 1 };
    std::vector<Bounds> text_bounds_; // 今の層の文字と数式
    Style root_style_;
    std::vector<const ScopedVars*> scoped_; // この図に当てる kLightScopedVars の行
};

std::string_view PeleSvgResolver::ScopedVarsFor(const XmlNode& el) const
{
    if (scoped_.empty()) {
        return {};
    }
    const auto classes = el.Attr("class");
    if (!classes) {
        return {};
    }
    for (const ScopedVars* scope : scoped_) {
        if (HasClass(*classes, scope->cls)) {
            return scope->vars;
        }
    }
    return {};
}

bool PeleSvgResolver::Apply(Style& s, const Style& parent, Prop prop, std::string_view value)
{
    value = Trim(value);
    // 末尾の !important だけを切る。値の中の ! (書体名など) では切らない。
    if (const size_t bang = value.rfind('!'); bang != std::string_view::npos && ascii_util::iequal(Trim(value.substr(bang + 1)), "important")) {
        value = Trim(value.substr(0, bang));
    }
    if (value.empty()) {
        return false;
    }
    if (ascii_util::iequal(value, "inherit")) {
        // 継承するものは既に親の値。継承しないものは親から写す。
        switch (prop) {
        case Prop::Opacity:
            s.own.opacity = parent.own.opacity;
            break;
        case Prop::BaselineShift:
            s.own.baseline_shift = parent.own.baseline_shift;
            break;
        case Prop::Rx:
            s.own.rx = parent.own.rx;
            break;
        case Prop::Ry:
            s.own.ry = parent.own.ry;
            break;
        case Prop::StopColor:
            s.own.stop_color = parent.own.stop_color;
            break;
        case Prop::StopOpacity:
            s.own.stop_opacity = parent.own.stop_opacity;
            break;
        default:
            break;
        }
        return true;
    }
    // keywords のうち value と一致したもの (大文字小文字は無視)。無ければ空。
    const auto keyword = [value](std::initializer_list<std::string_view> keywords) {
        const auto it = std::ranges::find_if(keywords, [value](std::string_view k) { return IEquals(value, k); });
        return it != keywords.end() ? *it : std::string_view{};
    };
    switch (prop) {
    case Prop::Color:
        return ParseColor(value, parent.color, s.color);
    case Prop::FontSize:
        // 計算値で丸めるので、文字の幅も描く大きさで測り、子の em も丸めた値から求める。
        if (const auto f = ParseFontSize(value, parent.font_size)) {
            s.font_size = std::min(*f, mermaid_util::kMaxFontSize);
            return true;
        }
        return false;
    case Prop::Fill:
    case Prop::Stroke: {
        Paint& paint = prop == Prop::Fill ? s.fill : s.stroke;
        if (!ParsePaint(value, s.color, paint)) {
            return false;
        }
        if (paint.kind == Paint::Kind::Url) {
            paint.url = Retain(paint.url);
        }
        return true;
    }
    case Prop::FillOpacity:
        return ParseOpacityValue(value, s.fill_opacity);
    case Prop::StrokeOpacity:
        return ParseOpacityValue(value, s.stroke_opacity);
    case Prop::Opacity:
        return ParseOpacityValue(value, s.own.opacity);
    case Prop::StopOpacity:
        return ParseOpacityValue(value, s.own.stop_opacity);
    case Prop::StopColor:
        return ParseColor(value, s.color, s.own.stop_color);
    case Prop::StrokeWidth:
        if (const auto f = ParseLength(value, s.font_size); f && *f >= 0.0f) {
            s.stroke_width = *f;
            return true;
        }
        return false;
    case Prop::StrokeDashoffset:
        if (const auto f = ParseLength(value, s.font_size)) {
            s.dash_offset = *f;
            return true;
        }
        return false;
    case Prop::StrokeMiterlimit:
        if (const auto f = ParseLength(value, s.font_size); f && *f >= 1.0f) {
            s.miter_limit = *f;
            return true;
        }
        return false;
    case Prop::StrokeDasharray: {
        if (ascii_util::iequal(value, "none")) {
            s.dash_array = {};
            return true;
        }
        std::string normalized;
        bool any = false;
        for (const auto part : FunctionTokens(value)) {
            const auto f = ParseLength(part, s.font_size);
            if (!f || *f < 0.0f) {
                return false;
            }
            any = any || *f > 0.0f;
            if (!normalized.empty()) {
                normalized += ' ';
            }
            AppendNum(normalized, *f);
        }
        if (normalized.empty()) {
            return false;
        }
        s.dash_array = any ? Keep(std::move(normalized)) : std::string_view{};
        return true;
    }
    case Prop::StrokeLinecap:
    case Prop::StrokeLinejoin: {
        const auto k = prop == Prop::StrokeLinecap ? keyword({ "butt", "round", "square" }) : keyword({ "miter", "round", "bevel" });
        if (k.empty()) {
            return false;
        }
        (prop == Prop::StrokeLinecap ? s.line_cap : s.line_join) = k;
        return true;
    }
    case Prop::FillRule: {
        const auto k = keyword({ "nonzero", "evenodd" });
        if (k.empty()) {
            return false;
        }
        s.even_odd = k == "evenodd";
        return true;
    }
    case Prop::Display:
        // 描き方を変えるのは none だけ。ほかの値 (inline・block など) の読み分けはしない。
        s.own.display_none = ascii_util::iequal(value, "none");
        return true;
    case Prop::Visibility: {
        const auto k = keyword({ "visible", "hidden", "collapse" });
        if (k.empty()) {
            return false;
        }
        s.visible = k == "visible";
        return true;
    }
    case Prop::FontFamily:
        s.font_family = wide_strings_.emplace_back(string_convert::Utf8ToWide(value));
        return true;
    case Prop::FontWeight: {
        std::string_view rest = value;
        double w = 0.0;
        if (ascii_util::iequal(value, "normal")) {
            s.weight = 400;
        }
        else if (ascii_util::iequal(value, "bold")) {
            s.weight = 700;
        }
        else if (const uint16_t relative = RelativeWeight(value, parent.weight)) {
            s.weight = relative;
        }
        else if (TakeNumber(rest, w) && rest.empty() && w >= 1.0 && w <= 1000.0) {
            s.weight = static_cast<uint16_t>(w);
        }
        else {
            return false;
        }
        return true;
    }
    case Prop::FontStyle: {
        const auto k = keyword({ "normal", "italic" });
        if (k.empty() && !ascii_util::istarts_with(value, "oblique")) {
            return false;
        }
        s.italic = k != "normal";
        return true;
    }
    case Prop::TextAnchor: {
        const auto k = keyword({ "start", "middle", "end" });
        if (k.empty()) {
            return false;
        }
        s.anchor = k == "middle" ? 1 : k == "end" ? 2
                                                  : 0;
        return true;
    }
    case Prop::TextDecoration:
        // 線の種類のほかに色・太さ・線の形も書けるので、読み分けはせず線の種類だけを拾う。
        s.own.decoration = (value.contains("underline") ? mermaid_util::kTextUnderline : 0) |
                           (value.contains("line-through") ? mermaid_util::kTextLineThrough : 0);
        return true;
    case Prop::BaselineShift:
        // 値は文字の大きさが決まってから BaselineShift が読む。
        s.own.baseline_shift = Retain(value);
        return true;
    case Prop::Rx:
    case Prop::Ry: {
        auto& r = prop == Prop::Rx ? s.own.rx : s.own.ry;
        if (ascii_util::iequal(value, "auto")) {
            r = std::nullopt;
            return true;
        }
        const auto f = ParseLength(value, s.font_size);
        if (!f || *f < 0.0f) {
            return false;
        }
        r = f;
        return true;
    }
    case Prop::MaxWidth:
        s.own.fit_width = value == "100%";
        return true;
    case Prop::Count:
        break;
    }
    return false;
}

Style PeleSvgResolver::Compute(const Style& parent, const XmlNode& el, std::string_view extra_style)
{
    Style s = parent;
    s.own = {};

    // 表示の属性 < style 属性。style の中では後の宣言が勝つ。読めない宣言は CSS と同じく捨て、同じ性質の前の宣言 (無ければ
    // 表示の属性、それも無ければ継承した値か初期値) を使う。var() を含む宣言は解くまで読めるか決まらないのでそのまま勝ち、
    // 解けないか読めなければ計算値の時点で無効 (継承するものは親の値、しないものは初期値) になる。
    declared_.clear();
    std::array<bool, kPropCount> has{};
    const auto set = [&](Prop p, std::string_view v) {
        declared_.emplace_back(p, v);
        has[static_cast<size_t>(p)] = true;
    };
    std::optional<std::string_view> style;
    for (const auto& attr : el.attrs) {
        if (attr.name == "xml:space") {
            s.preserve_space = attr.value == "preserve";
        }
        else if (attr.name == "style") {
            style = style ? style : attr.value;
        }
        else if (const auto p = PropOf(attr.name)) {
            set(*p, attr.value);
        }
    }
    Vars* own_vars = nullptr;
    const auto declaration = [&](std::string_view name, std::string_view value) {
        if (name.starts_with("--")) {
            if (!own_vars) {
                own_vars = &vars_.emplace_back(Vars{ .parent = parent.vars });
            }
            own_vars->values.emplace_back(name, value);
        }
        else if (ascii_util::iequal(name, "font")) {
            // [style] [weight] size[/line-height] family。大きさが無ければ宣言ごと読めない。
            std::string_view rest = value;
            std::string_view font_style = "normal";
            std::string_view font_weight = "normal";
            while (!rest.empty()) {
                const size_t end = std::min(rest.find_first_of(kSpaces), rest.size());
                const auto token = rest.substr(0, end);
                const auto size_token = token.substr(0, token.find('/'));
                if (ParseFontSize(size_token, parent.font_size)) {
                    set(Prop::FontStyle, font_style);
                    set(Prop::FontWeight, font_weight);
                    set(Prop::FontSize, size_token);
                    set(Prop::FontFamily, Trim(rest.substr(end)));
                    break;
                }
                if (ascii_util::iequal(token, "italic") || ascii_util::iequal(token, "oblique")) {
                    font_style = token;
                }
                else if (!ascii_util::iequal(token, "normal")) {
                    font_weight = token;
                }
                rest = Trim(rest.substr(end));
            }
        }
        else if (const auto p = PropOf(name)) {
            set(*p, value);
        }
        return true;
    };
    if (!extra_style.empty()) {
        ForEachDeclaration(extra_style, declaration);
    }
    if (style) {
        ForEachDeclaration(*style, declaration);
    }
    if (own_vars) {
        s.vars = own_vars;
    }
    // 性質は Prop の順に当てる (color と font-size を先に)。
    for (size_t i = 0; i < kPropCount; ++i) {
        if (!has[i]) {
            continue;
        }
        const auto prop = static_cast<Prop>(i);
        for (const auto& [p, value] : declared_ | std::views::reverse) {
            if (p != prop) {
                continue;
            }
            if (FindVar(value) != std::string_view::npos) {
                resolved_.clear();
                if (SubstituteVars(value, s.vars, resolved_)) {
                    Apply(s, parent, prop, resolved_);
                }
                break;
            }
            if (Apply(s, parent, prop, value)) {
                break;
            }
        }
    }
    return s;
}

// 塗りの属性。D2D の SVG はグラデーションで塗るときの fill-opacity / stroke-opacity を無視するので、止まりの不透明度に掛ける。
void PeleSvgResolver::AppendPaint(std::string& out, std::string_view name, const Paint& paint, const Style& s, float paint_opacity)
{
    std::optional<Rgba> color = SolidColor(paint, s);
    if (paint.kind == Paint::Kind::Url) {
        if (const auto gradient = GradientRef(paint.url, paint_opacity); !gradient.empty()) {
            out += ' ';
            out += name;
            out += "=\"url(#";
            out += gradient;
            out += ")\"";
            return;
        }
        if (paint.has_fallback) {
            color = paint.color;
        }
    }
    if (!color) {
        out += ' ';
        out += name;
        out += "=\"none\"";
        return;
    }
    AppendAttr(out, name, *color);
    AppendAttrUnlessInitial(out, name == "fill" ? "fill-opacity"sv : "stroke-opacity"sv, color->a * paint_opacity, 1.0);
}

std::string_view PeleSvgResolver::GradientRef(std::string_view id, float alpha)
{
    std::string key{ id };
    key += '|';
    AppendNum(key, alpha);
    if (const auto it = std::ranges::find(gradients_, key, &std::pair<std::string, std::string>::first); it != gradients_.end()) {
        return it->second;
    }
    if (!std::exchange(ids_ready_, true)) {
        for (uint32_t i = 0; i < nodes_.size(); ++i) {
            if (const auto node_id = nodes_[i].Attr("id")) {
                ids_.emplace(*node_id, i);
            }
        }
    }
    const auto found = ids_.find(id);
    const XmlNode* g = found == ids_.end() ? nullptr : &nodes_[found->second];
    if (!g || (g->name != "linearGradient" && g->name != "radialGradient")) {
        gradients_.emplace_back(std::move(key), std::string{});
        return {};
    }
    std::string name = "g" + std::to_string(gradients_.size());
    defs_ += '<';
    defs_ += g->name;
    AppendAttr(defs_, "id", name);
    for (const std::string_view attr : { "gradientUnits"sv, "gradientTransform"sv, "spreadMethod"sv, "x1"sv, "y1"sv, "x2"sv, "y2"sv, "cx"sv,
                                         "cy"sv, "r"sv, "fx"sv, "fy"sv }) {
        if (const auto value = g->Attr(attr)) {
            AppendAttr(defs_, attr, *value);
        }
    }
    defs_ += '>';
    for (const uint32_t child : g->children) {
        const XmlNode& stop = nodes_[child];
        if (stop.name != "stop") {
            continue;
        }
        const Style ss = Compute(root_style_, stop);
        defs_ += "<stop";
        AppendAttr(defs_, "offset", stop.Attr("offset").value_or("0"));
        AppendAttr(defs_, "stop-color", ss.own.stop_color);
        AppendAttr(defs_, "stop-opacity", ss.own.stop_color.a * ss.own.stop_opacity * alpha);
        defs_ += "/>";
    }
    defs_ += "</";
    defs_ += g->name;
    defs_ += '>';
    gradients_.emplace_back(std::move(key), std::move(name));
    return gradients_.back().second;
}

bool PeleSvgResolver::Shape(const XmlNode& el, const Style& s, const Mat& ctm, float opacity)
{
    const auto length = [&](std::string_view name) { return AttrLength(el, name, s.font_size).value_or(0.0f); };
    // rx・ry の auto はもう片方の値になる。
    const std::optional<float> rx = s.own.rx ? s.own.rx : s.own.ry;
    const std::optional<float> ry = s.own.ry ? s.own.ry : s.own.rx;
    const bool path = el.name == "path";
    Bounds local; // path と polygon は ShapesFor が要るときに求める
    std::array<std::pair<std::string_view, float>, 4> numbers{};
    size_t count = 0;
    std::optional<std::string_view> raw; // path の d・polygon の points
    if (path) {
        raw = el.Attr("d").value_or("");
    }
    else if (el.name == "rect") {
        const float x = length("x"), y = length("y"), w = length("width"), h = length("height");
        if (!(w > 0.0f && h > 0.0f)) {
            return true;
        }
        numbers = {
            { { "x", x }, { "y", y }, { "width", w }, { "height", h } }
        };
        count = 4;
        local.Add(x, y);
        local.Add(x + w, y + h);
    }
    else if (el.name == "circle") {
        const float cx = length("cx"), cy = length("cy"), r = length("r");
        if (!(r > 0.0f)) {
            return true;
        }
        numbers = {
            { { "cx", cx }, { "cy", cy }, { "r", r } }
        };
        count = 3;
        local.Add(cx - r, cy - r);
        local.Add(cx + r, cy + r);
    }
    else if (el.name == "ellipse") {
        const float cx = length("cx"), cy = length("cy");
        if (!(rx.value_or(0.0f) > 0.0f && ry.value_or(0.0f) > 0.0f)) {
            return true;
        }
        numbers = {
            { { "cx", cx }, { "cy", cy }, { "rx", *rx }, { "ry", *ry } }
        };
        count = 4;
        local.Add(cx - *rx, cy - *ry);
        local.Add(cx + *rx, cy + *ry);
    }
    else if (el.name == "line") {
        numbers = {
            { { "x1", length("x1") }, { "y1", length("y1") }, { "x2", length("x2") }, { "y2", length("y2") } }
        };
        count = 4;
        local.Add(numbers[0].second, numbers[1].second);
        local.Add(numbers[2].second, numbers[3].second);
    }
    else {
        raw = el.Attr("points").value_or("");
    }

    std::string& out = ShapesFor([&] {
        const Bounds b = !raw ? local : path ? PathBounds(*raw)
                                             : PointsBounds(*raw);
        return b.Transformed(ctm).Padded(s.stroke_width);
    });
    out += '<';
    out += el.name;
    for (size_t i = 0; i < count; ++i) {
        AppendAttr(out, numbers[i].first, numbers[i].second);
    }
    if (raw) {
        AppendAttr(out, path ? "d"sv : "points"sv, *raw);
    }
    if (el.name == "rect" && rx) {
        AppendAttr(out, "rx", *rx);
        AppendAttr(out, "ry", *ry);
    }
    AppendTransform(out, ctm.Values());
    AppendPaint(out, "fill", s.fill, s, s.fill_opacity);
    if (s.even_odd) {
        out += " fill-rule=\"evenodd\"";
    }
    if (s.stroke.kind != Paint::Kind::None) {
        AppendPaint(out, "stroke", s.stroke, s, s.stroke_opacity);
        AppendAttrUnlessInitial(out, "stroke-width", s.stroke_width, 1.0);
        if (s.line_cap != "butt") {
            AppendAttr(out, "stroke-linecap", s.line_cap);
        }
        if (s.line_join != "miter") {
            AppendAttr(out, "stroke-linejoin", s.line_join);
        }
        AppendAttrUnlessInitial(out, "stroke-miterlimit", s.miter_limit, 4.0);
        if (!s.dash_array.empty()) {
            AppendAttr(out, "stroke-dasharray", s.dash_array);
            AppendAttrUnlessInitial(out, "stroke-dashoffset", s.dash_offset, 0.0);
        }
    }
    if (opacity < 1.0f) {
        AppendAttr(out, "opacity", opacity);
    }
    out += "/>";
    return true;
}

float PeleSvgResolver::BaselineShift(const Style& s)
{
    const auto v = s.own.baseline_shift;
    if (v.empty() || ascii_util::iequal(v, "baseline")) {
        return 0.0f;
    }
    if (ascii_util::iequal(v, "sub") || ascii_util::iequal(v, "super")) {
        // Chromium と同じく、その要素の書体の行の高さの半分だけずらす。
        const auto [ascent, descent] = measurer_.Extent(s.Font());
        const float half = (ascent + descent) / 2.0f;
        return ascii_util::iequal(v, "sub") ? half : -half;
    }
    const auto length = ParseLengthOrPercent(v, s.font_size, s.font_size);
    return length ? -*length : 0.0f;
}

bool PeleSvgResolver::CollectText(uint32_t index, const Style& s, float opacity, uint8_t decoration, float shift, std::vector<Piece>& pieces,
                                  std::optional<float>& pending_x, std::optional<float>& pending_y)
{
    const XmlNode& el = nodes_[index];
    // 文字ごとの位置 (値の並び) と回転は DirectWrite の 1 行では置けない。
    for (const std::string_view unsupported : { "dx"sv, "dy"sv, "rotate"sv, "textLength"sv }) {
        if (el.Attr(unsupported)) {
            return false;
        }
    }
    const auto position = [&](std::string_view name, std::optional<float>& out) {
        const auto v = el.Attr(name);
        if (!v) {
            return true;
        }
        std::array<double, 1> number{};
        const auto count = ParseNumbers(*v, number);
        if (!count) {
            return false;
        }
        if (*count == 1) {
            out = static_cast<float>(number[0]);
        }
        return true;
    };
    std::optional<float> own_x;
    std::optional<float> own_y;
    if (!position("x", own_x) || !position("y", own_y)) {
        return false;
    }
    const bool positioned = own_x || own_y;
    if (positioned) {
        pending_x = own_x;
        pending_y = own_y;
    }
    for (const uint32_t child : el.children) {
        const XmlNode& node = nodes_[child];
        if (node.name.empty()) {
            pieces.push_back({ .raw = node.text,
                               .style = s,
                               .opacity = opacity,
                               .decoration = decoration,
                               .shift = shift,
                               .x = std::exchange(pending_x, std::nullopt),
                               .y = std::exchange(pending_y, std::nullopt) });
            continue;
        }
        if (node.name == "title" || node.name == "desc") {
            continue;
        }
        if (node.name != "tspan") {
            return false;
        }
        const Style ts = Compute(s, node);
        if (ts.own.display_none) {
            continue;
        }
        if (!CollectText(child, ts, opacity * ts.own.opacity, decoration | ts.own.decoration, shift + BaselineShift(ts), pieces, pending_x, pending_y)) {
            return false;
        }
    }
    // 文字の無い要素の位置は捨てる (位置は文字に付く)。
    if (positioned) {
        pending_x.reset();
        pending_y.reset();
    }
    return true;
}

bool PeleSvgResolver::Text(uint32_t index, const Style& s, const Mat& ctm, float opacity)
{
    std::vector<Piece> pieces;
    std::optional<float> pending_x;
    std::optional<float> pending_y;
    if (!CollectText(index, s, opacity, s.own.decoration, 0.0f, pieces, pending_x, pending_y)) {
        return false;
    }
    // 空白。xml:space="preserve" は改行とタブを空白にするだけ。それ以外は CSS の white-space: normal と同じく
    // 続く空白を 1 つに畳み、要素の先頭と末尾の空白を除く。
    bool after_space = true;
    std::string folded;
    for (Piece& piece : pieces) {
        folded.clear();
        for (const char c : piece.raw) {
            if (piece.style.preserve_space) {
                folded += IsSpace(c) ? ' ' : c;
                after_space = false;
            }
            else if (!IsSpace(c)) {
                folded += c;
                after_space = false;
            }
            else if (!after_space) {
                folded += ' ';
                after_space = true;
            }
        }
        piece.text = string_convert::Utf8ToWide(folded);
    }
    for (auto it = pieces.rbegin(); it != pieces.rend() && !it->style.preserve_space; ++it) {
        while (!it->text.empty() && it->text.back() == L' ') {
            it->text.pop_back();
        }
        if (!it->text.empty()) {
            break;
        }
    }
    // 空になった並びの位置は次の並びへ送る。
    for (size_t i = 0; i + 1 < pieces.size(); ++i) {
        if (pieces[i].text.empty()) {
            pieces[i + 1].x = pieces[i + 1].x ? pieces[i + 1].x : pieces[i].x;
            pieces[i + 1].y = pieces[i + 1].y ? pieces[i + 1].y : pieces[i].y;
        }
    }
    std::erase_if(pieces, [](const Piece& p) { return p.text.empty(); });

    // 塊 (x か y が指定された文字から次の指定まで) ごとに、text-anchor で幅の分だけ戻す。
    double x = 0.0;
    double y = 0.0;
    size_t chunk_start = 0;
    double chunk_left = 0.0;
    const auto finish_chunk = [&](size_t end) {
        if (chunk_start >= end) {
            return;
        }
        const uint8_t anchor = pieces[chunk_start].style.anchor;
        const double dx = anchor == 1 ? -(x - chunk_left) / 2.0 : anchor == 2 ? -(x - chunk_left)
                                                                              : 0.0;
        for (size_t k = chunk_start; k < end; ++k) {
            pieces[k].left = static_cast<float>(pieces[k].left + dx);
        }
        x += dx;
    };
    for (size_t i = 0; i < pieces.size(); ++i) {
        Piece& piece = pieces[i];
        if (i == 0 || piece.x || piece.y) {
            finish_chunk(i);
            x = piece.x.value_or(static_cast<float>(x));
            y = piece.y.value_or(static_cast<float>(y));
            chunk_start = i;
            chunk_left = x;
        }
        piece.width = measurer_.Width(piece.text, piece.style.Font());
        piece.left = static_cast<float>(x);
        piece.top = static_cast<float>(y);
        x += piece.width;
    }
    finish_chunk(pieces.size());

    const auto transform = ToArray(ctm);
    for (Piece& piece : pieces) {
        const Style& ps = piece.style;
        const auto color = SolidColor(ps.fill, ps);
        if (piece.text.find_first_not_of(L' ') == std::wstring::npos || !ps.visible || piece.opacity <= 0.0f || !color) {
            continue;
        }
        const mermaid_util::DrawingPlacement at{
            .transform = transform,
            .x = piece.left,
            .y = piece.top + piece.shift,
            .font_size = ps.font_size,
            .rgb = ToRgb(*color),
            .alpha = std::clamp(color->a * ps.fill_opacity * piece.opacity, 0.0f, 1.0f),
        };
        // 大きさ 0 の文字や、変換と位置が float に収まらない文字は描けない。書くとディスクキャッシュから読み戻せず、開くたびに描き直す。
        if (!mermaid_util::IsStorable(at)) {
            continue;
        }
        const auto [ascent, descent] = measurer_.Extent(ps.Font());
        Bounds box;
        box.Add(at.x, at.y - ascent);
        box.Add(at.x + piece.width, at.y + descent);
        text_bounds_.push_back(box.Transformed(ctm));
        layers_.back().texts.push_back({
            .at = at,
            .weight = ps.weight,
            .italic = ps.italic,
            .decoration = piece.decoration,
            .family = std::wstring{ ps.font_family },
            .text = std::move(piece.text),
        });
    }
    return true;
}

// <svg class="pele-math" x y width height color opacity><g data-mendo-math="番号"/></svg>。pele は箱を行の中央に置く。
bool PeleSvgResolver::MathLabel(const XmlNode& el, const Style& s, const Mat& ctm, float opacity)
{
    const auto number = [&]() -> std::optional<size_t> {
        if (el.children.size() != 1) {
            return std::nullopt;
        }
        const auto attr = nodes_[el.children[0]].Attr("data-mendo-math");
        size_t n = 0;
        if (!attr || std::from_chars(attr->data(), attr->data() + attr->size(), n).ec != std::errc{} || n >= maths_.size()) {
            return std::nullopt;
        }
        return n;
    }();
    if (!number || !maths_[*number].drawing) {
        return false;
    }
    const PeleMathLabel& label = maths_[*number];
    const MathDrawing& math = *label.drawing;
    const float x = AttrLength(el, "x", s.font_size).value_or(0.0f);
    const float top = AttrLength(el, "y", s.font_size).value_or(0.0f);
    const mermaid_util::DrawingPlacement at{
        .transform = ToArray(ctm),
        .x = x,
        .y = top + math.height,
        .font_size = label.font_size,
        .rgb = ToRgb(s.color),
        .alpha = std::clamp(s.color.a * opacity, 0.0f, 1.0f),
    };
    // 文字と同じく、ディスクキャッシュに書けない数式は描かない。
    if (!mermaid_util::IsStorable(at)) {
        return true;
    }
    Bounds box;
    box.Add(x, top);
    box.Add(x + math.width, top + math.height + math.depth);
    text_bounds_.push_back(box.Transformed(ctm));
    layers_.back().maths.push_back({
        .at = at,
        .tex = label.tex,
        .drawing = label.drawing,
    });
    return true;
}

bool PeleSvgResolver::Walk(uint32_t index, const Style& parent, const Mat& ctm, float opacity)
{
    const XmlNode& el = nodes_[index];
    const std::string_view tag = el.name;
    if (tag.empty() || tag == "defs" || tag == "title" || tag == "desc" || tag == "metadata" || tag == "linearGradient" ||
        tag == "radialGradient" || tag == "symbol") {
        return true;
    }
    const bool shape = tag == "path" || tag == "rect" || tag == "circle" || tag == "ellipse" || tag == "line" || tag == "polyline" || tag == "polygon";
    const bool container = tag == "g" || tag == "svg" || tag == "a";
    if (!shape && !container && tag != "text") {
        // foreignObject・image・use・style など。一部だけ描いた図は誤解を招くので、図ごと描かない。
        return false;
    }
    const Style s = Compute(parent, el, ScopedVarsFor(el));
    const float own_opacity = opacity * s.own.opacity;
    if (s.own.display_none || own_opacity <= 0.0f) {
        return true;
    }
    Mat m = ctm;
    if (const auto transform = el.Attr("transform")) {
        Mat t;
        if (ParseTransform(*transform, t)) {
            m = Compose(m, t);
        }
    }
    if (tag == "svg" && el.Attr("class") == "pele-math"sv) {
        return MathLabel(el, s, m, own_opacity);
    }
    if (tag == "svg") {
        const auto w = AttrLength(el, "width", s.font_size);
        const auto h = AttrLength(el, "height", s.font_size);
        Mat viewport;
        if (!w || !h ||
            !ViewportTransform(el, AttrLength(el, "x", s.font_size).value_or(0.0f), AttrLength(el, "y", s.font_size).value_or(0.0f), *w, *h, viewport)) {
            return false;
        }
        m = Compose(m, viewport);
    }
    if (container) {
        return std::ranges::all_of(el.children, [&](uint32_t child) { return Walk(child, s, m, own_opacity); });
    }
    if (tag == "text") {
        return Text(index, s, m, own_opacity);
    }
    return !s.visible || Shape(el, s, m, own_opacity);
}

std::optional<mermaid_util::DiagramDrawing> PeleSvgResolver::Run(bool dark, float max_width)
{
    const XmlNode& root = nodes_[0];
    if (root.name != "svg") {
        return std::nullopt;
    }
    if (!dark) {
        const auto root_classes = root.Attr("class").value_or(""sv);
        for (const auto& scope : kLightScopedVars) {
            if (scope.diagram.empty() || HasClass(root_classes, scope.diagram)) {
                scoped_.push_back(&scope);
            }
        }
    }
    root_style_ = Compute(Style{}, root, dark ? kDarkVars : kLightVars);
    const Style& rs = root_style_;
    const auto attr_w = AttrLength(root, "width", rs.font_size);
    const auto attr_h = AttrLength(root, "height", rs.font_size);
    if (!(attr_w.value_or(0.0f) > 0.0f && attr_h.value_or(0.0f) > 0.0f)) {
        return std::nullopt;
    }
    float w = *attr_w;
    float h = *attr_h;
    // ブラウザと同じく SVG の style の max-width:100%;height:auto で、幅の上限まで縦横比を保って縮める。
    if (rs.own.fit_width && max_width > 0.0f && w > max_width) {
        h = h * max_width / w;
        w = max_width;
    }
    // ブラウザのレイアウトは 1/64 px に切り捨てる。大きさの切り上げをブラウザとそろえる (837x520 を幅 800 にすると高さ 497.013 → 497)。
    w = std::floor(w * 64.0f) / 64.0f;
    h = std::floor(h * 64.0f) / 64.0f;
    Mat root_ctm;
    if (!ViewportTransform(root, 0.0, 0.0, w, h, root_ctm)) {
        return std::nullopt;
    }
    if (!rs.own.display_none && !std::ranges::all_of(root.children, [&](uint32_t child) { return Walk(child, rs, root_ctm, rs.own.opacity); })) {
        return std::nullopt;
    }
    return mermaid_util::DiagramDrawing{
        .width = std::ceil(w),
        .height = std::ceil(h),
        .background = dark ? kDiagramDarkBackground : kDiagramLightBackground,
        .defs = std::move(defs_),
        .layers = std::move(layers_),
    };
}

// ---------------------------------------------------------------------------------------------------------------
// KaTeX の HTML 出力の配置。katex.css が使う範囲の CSS (vlist の表・inline-block・相対位置・絶対位置の SVG) だけを解く。
// KaTeX は縦の位置 (vlist の top と pstrut) と高さを自分で決めて em で書くので、ブラウザに頼るのは横の並びと字幅だけ。
// KaTeX の書体は GSUB・GPOS・kern の表を持たないので、字幅は hmtx の送り幅の和になる (Chrome の HarfBuzz と同じ)。

enum class KFamily : uint8_t {
    Main,
    Math,
    AMS,
    Caligraphic,
    Fraktur,
    SansSerif,
    Script,
    Size1,
    Size2,
    Size3,
    Size4,
    Typewriter
};

struct KFaceEntry {
    KFamily family;
    bool bold;
    bool italic;
};
// kKatexFontFiles と同じ順。
constexpr KFaceEntry kKatexFaces[]{
    { KFamily::AMS,         false, false },
    { KFamily::Caligraphic, true,  false },
    { KFamily::Caligraphic, false, false },
    { KFamily::Fraktur,     true,  false },
    { KFamily::Fraktur,     false, false },
    { KFamily::Main,        true,  false },
    { KFamily::Main,        true,  true  },
    { KFamily::Main,        false, true  },
    { KFamily::Main,        false, false },
    { KFamily::Math,        true,  true  },
    { KFamily::Math,        false, true  },
    { KFamily::SansSerif,   true,  false },
    { KFamily::SansSerif,   false, true  },
    { KFamily::SansSerif,   false, false },
    { KFamily::Script,      false, false },
    { KFamily::Size1,       false, false },
    { KFamily::Size2,       false, false },
    { KFamily::Size3,       false, false },
    { KFamily::Size4,       false, false },
    { KFamily::Typewriter,  false, false },
};
static_assert(std::size(kKatexFaces) == std::size(kKatexFontFiles));

struct KFont {
    KFamily family = KFamily::Main;
    bool bold = false;
    bool italic = false;
};

// CSS の書体の選び方。字形の傾きを先に合わせ、無ければ斜体を合成する。太さが無ければ太字を合成する。
// 斜体しか無い書体 (KaTeX_Math) に立体を求めたときは、合成せずに斜体を使う。
std::pair<size_t, uint32_t> MatchKatexFace(const KFont& font) noexcept
{
    const auto has = [&](bool italic) {
        return std::ranges::any_of(kKatexFaces, [&](const KFaceEntry& e) { return e.family == font.family && e.italic == italic; });
    };
    const bool style_found = has(font.italic);
    const bool italic = style_found ? font.italic : !font.italic;
    uint32_t sim = font.italic && !style_found ? DWRITE_FONT_SIMULATIONS_OBLIQUE : 0;
    size_t best = std::size(kKatexFaces);
    for (size_t i = 0; i < std::size(kKatexFaces); ++i) {
        const auto& e = kKatexFaces[i];
        if (e.family != font.family || e.italic != italic) {
            continue;
        }
        if (best == std::size(kKatexFaces) || e.bold == font.bold) {
            best = i;
        }
    }
    if (best == std::size(kKatexFaces)) {
        return { 8, 0 }; // Main-Regular
    }
    if (font.bold && !kKatexFaces[best].bold) {
        sim |= DWRITE_FONT_SIMULATIONS_BOLD;
    }
    return { best, sim };
}

enum class KAlign : uint8_t {
    Left,
    Center,
    Right
};

// 継承する値。
struct KInherit {
    float font_size = 16.0f;
    KFont font;
    KAlign align = KAlign::Center;
    std::optional<std::pair<float, float>> shadow; // text-shadow の x, y
    MathDrawing::Color color;
};

struct KLen {
    float v = 0.0f;
    bool pct = false; // v は 0〜1
};

enum class KDisplay : uint8_t {
    Inline,
    InlineBlock,
    Block
};
enum class KPosition : uint8_t {
    Static,
    Relative,
    Absolute
};

// 継承しない値。長さは CSS px に直してある (% を除く)。
struct KOwn {
    KDisplay display = KDisplay::Inline;
    KPosition position = KPosition::Static;
    std::optional<KLen> width;
    std::optional<float> height;
    std::optional<float> min_width;
    float min_height = 0.0f;
    float margin_l = 0.0f, margin_r = 0.0f;
    float padding_l = 0.0f, padding_r = 0.0f;
    std::array<float, 4> border{}; // top, right, bottom, left
    std::array<bool, 4> dashed{};
    bool border_box = false;
    std::optional<float> vertical_align; // 上向きが正
    std::optional<KLen> top, left, right, bottom;
    bool reflect = false;
    MathDrawing::Color background, border_color; // border_color が無ければ color (currentColor)
};

// 要素の計算値。inh は子が継ぐ値、own はその要素だけの値。
struct KStyle {
    KInherit inh;
    KOwn own;
};

constexpr float kKatexSizes[]{ 0.5f, 0.6f, 0.7f, 0.8f, 0.9f, 1.0f, 1.2f, 1.44f, 1.728f, 2.074f, 2.488f };

struct KBox {
    float w = 0.0f;
    float asc = 0.0f;
    float desc = 0.0f;
    bool has_content = false; // 行の中身 (文字・inline-block) がある。無い inline-block のベースラインは下端
    bool pct = false;         // 含む塊の幅に依る。幅が決まってから置き直す
    std::vector<MathDrawing::Item> items;
};

// 百分率の基準。nullopt は「まだ決まっていない」(列の幅を求める 1 回目)。
struct KCtx {
    std::optional<float> cb_w;     // 含む塊の幅
    std::optional<float> pos_cb_w; // 位置指定した祖先の幅 (絶対位置の SVG の width:100%)
};

void TranslateItems(std::vector<MathDrawing::Item>& items, float dx, float dy)
{
    if (dx == 0.0f && dy == 0.0f) {
        return;
    }
    for (auto& item : items) {
        std::visit(
            [&](auto& it) {
            using T = std::decay_t<decltype(it)>;
            if constexpr (std::is_same_v<T, MathDrawing::Path>) {
                it.transform[4] += dx;
                it.transform[5] += dy;
                it.clip[0] += dx;
                it.clip[1] += dy;
                it.clip[2] += dx;
                it.clip[3] += dy;
            }
            else {
                it.x += dx;
                it.y += dy;
            }
        },
            item);
    }
}

void AppendItems(KBox& out, KBox&& box, float dx, float dy)
{
    TranslateItems(box.items, dx, dy);
    if (out.items.empty()) {
        out.items = std::move(box.items);
    }
    else {
        out.items.append_range(box.items | std::views::as_rvalue);
    }
}

bool HasAnyClass(std::string_view classes, std::initializer_list<std::string_view> names) noexcept
{
    return AnyClass(classes, [names](std::string_view c) { return std::ranges::contains(names, c); });
}

// "reset-size6" や "size3" の番号。無ければ 0。
int SizeClass(std::string_view classes, std::string_view prefix) noexcept
{
    int found = 0;
    AnyClass(classes, [&](std::string_view c) {
        int n = 0;
        const auto digits = c.substr(std::min(prefix.size(), c.size()));
        const auto [ptr, ec] = std::from_chars(digits.data(), digits.data() + digits.size(), n);
        if (!c.starts_with(prefix) || ec != std::errc{} || ptr != digits.data() + digits.size() || n < 1 || n > 11) {
            return false;
        }
        found = n;
        return true;
    });
    return found;
}

// 長さか百分率。em は font_size、% は KLen::pct (0〜1)。
std::optional<KLen> ParseKLen(std::string_view v, float font_size)
{
    v = Trim(v);
    if (const auto out = ParseLengthOrPercent(v, font_size, 1.0f)) {
        return KLen{ *out, v.ends_with('%') };
    }
    return std::nullopt;
}

// 足す順を変えない (std::reduce は順を決めないので、字の位置が前と変わりうる)。
float RunWidth(const MathDrawing::Glyphs& g) noexcept
{
    return std::accumulate(g.advances.begin(), g.advances.end(), 0.0f);
}

float AlignOffset(KAlign align, float slack) noexcept
{
    return slack <= 0.0f ? 0.0f : align == KAlign::Center ? slack / 2
                              : align == KAlign::Right    ? slack
                                                          : 0.0f;
}

// 箱を行の右に gap 空けて足す。
void AppendInline(KBox& row, KBox&& box, float gap = 0.0f)
{
    row.asc = std::max(row.asc, box.asc);
    row.desc = std::max(row.desc, box.desc);
    row.has_content |= box.has_content;
    row.pct |= box.pct;
    const float x = row.w + gap;
    row.w = x + box.w;
    AppendItems(row, std::move(box), x, 0.0f);
}

// 隣り合う字形の並びのうち、同じ書体・大きさ・ベースラインで左から右へ続くものを 1 本にする。KaTeX は記号を 1 つずつ
// span に入れるので、そのままだと x^2+y^2 で 9 本になり、毎フレームの DrawGlyphRun の数とメモリが増える。
// 間の空き (mspace など) は前の字の送り幅に足す。色の違う並びはつながず、つなぐのは直前に残した並びとだけなので、描く順は変わらない。
void MergeGlyphRuns(std::vector<MathDrawing::Item>& items)
{
    size_t kept = 0;
    for (size_t i = 0; i < items.size(); ++i) {
        auto* g = std::get_if<MathDrawing::Glyphs>(&items[i]);
        auto* prev = kept == 0 ? nullptr : std::get_if<MathDrawing::Glyphs>(&items[kept - 1]);
        if (g && prev && !g->mirror && !prev->mirror && prev->face == g->face && prev->size == g->size && prev->y == g->y && prev->color == g->color &&
            !prev->advances.empty()) {
            const float gap = g->x - (prev->x + RunWidth(*prev));
            if (gap >= -0.01f) {
                prev->advances.back() += std::max(gap, 0.0f);
                prev->glyphs.append_range(g->glyphs);
                prev->advances.append_range(g->advances);
                continue;
            }
        }
        if (kept != i) {
            items[kept] = std::move(items[i]);
        }
        ++kept;
    }
    // 詰め直し (shrink_to_fit) はしない。返るのは 1 式約 0.4KB で、927 式の文書でも約 0.4MB とアプリ全体の 1% に届かない
    // (test.md と代表的な式 104 本で 42.5KB、2026-11-06)。
    items.erase(items.begin() + static_cast<std::ptrdiff_t>(kept), items.end());
}

// UTF-8 の文字の並びを 1 文字ずつ。
template <class F>
void ForEachCp(std::string_view text, F&& f)
{
    for (uint32_t i = 0; i < text.size();) {
        const auto d = utf8_codec::DecodeAt(text, i);
        f(d.cp);
        i += std::max<uint32_t>(d.len, 1);
    }
}

ComPtr<ID2D1PathGeometry> BuildPathGeometry(ID2D1Factory* d2d, std::string_view d)
{
    ComPtr<ID2D1PathGeometry> geometry;
    ComPtr<ID2D1GeometrySink> sink;
    if (FAILED(d2d->CreatePathGeometry(&geometry)) || FAILED(geometry->Open(&sink))) {
        return nullptr;
    }
    sink->SetFillMode(D2D1_FILL_MODE_WINDING);
    struct Visitor {
        ID2D1GeometrySink* sink;
        D2D1_POINT_2F at{};
        bool open = false;

        static D2D1_POINT_2F Pt(double x, double y)
        {
            return D2D1::Point2F(static_cast<float>(x), static_cast<float>(y));
        }
        void Begin()
        {
            if (!open) {
                sink->BeginFigure(at, D2D1_FIGURE_BEGIN_FILLED);
                open = true;
            }
        }
        void End(D2D1_FIGURE_END end)
        {
            if (open) {
                sink->EndFigure(end);
                open = false;
            }
        }
        void Move(double x, double y)
        {
            End(D2D1_FIGURE_END_OPEN);
            at = Pt(x, y);
        }
        void Line(double x, double y)
        {
            Begin();
            sink->AddLine(at = Pt(x, y));
        }
        void Cubic(double x1, double y1, double x2, double y2, double x, double y)
        {
            Begin();
            sink->AddBezier(D2D1::BezierSegment(Pt(x1, y1), Pt(x2, y2), at = Pt(x, y)));
        }
        void Quad(double x1, double y1, double x, double y)
        {
            Begin();
            sink->AddQuadraticBezier(D2D1::QuadraticBezierSegment(Pt(x1, y1), at = Pt(x, y)));
        }
        void Arc(double, double, double rx, double ry, double rotation, bool large, bool sweep, double x2, double y2)
        {
            Begin();
            sink->AddArc(D2D1::ArcSegment(at = Pt(x2, y2), D2D1::SizeF(static_cast<float>(std::abs(rx)), static_cast<float>(std::abs(ry))),
                                          static_cast<float>(rotation), sweep ? D2D1_SWEEP_DIRECTION_CLOCKWISE : D2D1_SWEEP_DIRECTION_COUNTER_CLOCKWISE,
                                          large ? D2D1_ARC_SIZE_LARGE : D2D1_ARC_SIZE_SMALL));
        }
        void Close()
        {
            End(D2D1_FIGURE_END_CLOSED);
        }
    } visitor{ sink.Get() };
    const bool ok = WalkPath(d, visitor);
    visitor.End(D2D1_FIGURE_END_OPEN);
    if (FAILED(sink->Close()) || !ok) {
        return nullptr;
    }
    return geometry;
}

class KatexLayout {
public:
    KatexLayout(const XmlDoc& doc, KatexAssets& assets) : doc_(doc), assets_(assets)
    {
        eqn_number_.assign(doc.nodes.size(), 0);
        // 自動の式番号 (.eqn-num::before の CSS カウンタ)。数式ごとに 1 から、文書の順に振る。vlist は列の幅を決めてから
        // 置き直すことがあるので、配置の途中で数えずに先に決めておく。
        uint32_t eqn = 0;
        for (uint32_t i = 0; i < doc.nodes.size(); ++i) {
            if (HasClass(Classes(i), "eqn-num")) {
                eqn_number_[i] = ++eqn;
            }
        }
    }

    std::optional<MathDrawing> Run(float font_size);

private:
    std::string_view Classes(uint32_t i) const noexcept
    {
        return i == kNoNode ? std::string_view{} : doc_.nodes[i].Attr("class").value_or(std::string_view{});
    }
    KStyle Compute(uint32_t i, const KInherit& parent);
    KBox Element(uint32_t i, const KInherit& parent, const KCtx& ctx, bool force_inline_block = false);
    void Flow(uint32_t i, const KInherit& inh, const KCtx& ctx, KBox& out, std::vector<uint32_t>& absolutes);
    KBox VList(uint32_t i, const KInherit& inh);
    void Text(std::string_view text, const KInherit& inh, KBox& out);
    void Svg(uint32_t i, const KInherit& inh, float x, float y, float w, std::optional<float> h, KBox& out);
    bool IsAbsolute(uint32_t i) const noexcept;
    void Fail()
    {
        failed_ = true;
    }

    const XmlDoc& doc_;
    KatexAssets& assets_;
    std::vector<uint32_t> eqn_number_;
    bool failed_ = false;
};

bool KatexLayout::IsAbsolute(uint32_t i) const noexcept
{
    const auto& n = doc_.nodes[i];
    if (n.name == "svg") {
        return true;
    }
    return HasAnyClass(
        Classes(i),
        {
            "halfarrow-left",
            "halfarrow-right",
            "brace-left",
            "brace-center",
            "brace-right",
            "katex-inner",
            "cd-label-left",
            "cd-label-right",
        });
}

KStyle KatexLayout::Compute(uint32_t i, const KInherit& parent)
{
    KStyle result{ .inh = parent };
    auto& [inh, own] = result;
    const auto c = Classes(i);
    const uint32_t p = doc_.nodes[i].parent;
    const auto pc = Classes(p);
    const auto has = [&](std::string_view name) { return HasClass(c, name); };
    const auto any = [&](std::initializer_list<std::string_view> names) { return HasAnyClass(c, names); };

    // 文字の大きさ (.katex-sizing.reset-sizeA.sizeB)。em は親の大きさ。
    if (has("katex-sizing") || has("fontsize-ensurer")) {
        const int from = SizeClass(c, "reset-size");
        const int to = SizeClass(c, "size");
        if (from && to) {
            inh.font_size = parent.font_size * kKatexSizes[to - 1] / kKatexSizes[from - 1];
        }
    }
    // 書体 (katex.css の順。後の規則が勝つ)。
    struct FontRule {
        std::string_view cls;
        std::optional<KFamily> family;
        std::optional<bool> bold;
        std::optional<bool> italic;
    };
    static constexpr FontRule kFontRules[]{
        { "textbf",       {},                   true, {}    },
        { "textit",       {},                   {},   true  },
        { "textrm",       KFamily::Main,        {},   {}    },
        { "textsf",       KFamily::SansSerif,   {},   {}    },
        { "texttt",       KFamily::Typewriter,  {},   {}    },
        { "mathnormal",   KFamily::Math,        {},   true  },
        { "mathit",       KFamily::Main,        {},   true  },
        { "mathrm",       {},                   {},   false },
        { "mathbf",       KFamily::Main,        true, {}    },
        { "boldsymbol",   KFamily::Math,        true, true  },
        { "amsrm",        KFamily::AMS,         {},   {}    },
        { "mathbb",       KFamily::AMS,         {},   {}    },
        { "textbb",       KFamily::AMS,         {},   {}    },
        { "mathcal",      KFamily::Caligraphic, {},   {}    },
        { "mathfrak",     KFamily::Fraktur,     {},   {}    },
        { "textfrak",     KFamily::Fraktur,     {},   {}    },
        { "mathboldfrak", KFamily::Fraktur,     true, {}    },
        { "textboldfrak", KFamily::Fraktur,     true, {}    },
        { "mathtt",       KFamily::Typewriter,  {},   {}    },
        { "mathscr",      KFamily::Script,      {},   {}    },
        { "textscr",      KFamily::Script,      {},   {}    },
        { "mathsf",       KFamily::SansSerif,   {},   {}    },
        { "mathboldsf",   KFamily::SansSerif,   true, {}    },
        { "textboldsf",   KFamily::SansSerif,   true, {}    },
        { "mathsfit",     KFamily::SansSerif,   {},   true  },
        { "mathitsf",     KFamily::SansSerif,   {},   true  },
        { "textitsf",     KFamily::SansSerif,   {},   true  },
        { "mainrm",       KFamily::Main,        {},   false },
    };
    for (const auto& r : kFontRules) {
        if (has(r.cls)) {
            inh.font.family = r.family.value_or(inh.font.family);
            inh.font.bold = r.bold.value_or(inh.font.bold);
            inh.font.italic = r.italic.value_or(inh.font.italic);
        }
    }
    // 詳細度の高い規則 (.delimsizing.size1 など)。
    if (has("delimsizing")) {
        if (const int n = SizeClass(c, "size"); n >= 1 && n <= 4) {
            inh.font.family = static_cast<KFamily>(static_cast<int>(KFamily::Size1) + n - 1);
        }
    }
    if (has("op-symbol")) {
        if (has("small-op")) {
            inh.font.family = KFamily::Size1;
        }
        else if (has("large-op")) {
            inh.font.family = KFamily::Size2;
        }
    }
    if (doc_.nodes[i].name == "span" && (HasClass(pc, "delim-size1") || HasClass(pc, "delim-size4"))) {
        inh.font.family = HasClass(pc, "delim-size1") ? KFamily::Size1 : KFamily::Size4;
    }

    // text-align (継承する)。
    if (any({ "msupsub", "svg-align", "cd-label-left" })) {
        inh.align = KAlign::Left;
    }
    if (any({ "x-arrow", "mover", "munder" })) {
        inh.align = KAlign::Center;
    }
    if (has("cd-label-right")) {
        inh.align = KAlign::Right;
    }
    if (has("vlist-t")) {
        if (HasAnyClass(pc, { "op-limits", "katex-accent", "col-align-c" })) {
            inh.align = KAlign::Center;
        }
        else if (HasClass(pc, "col-align-l")) {
            inh.align = KAlign::Left;
        }
        else if (HasClass(pc, "col-align-r")) {
            inh.align = KAlign::Right;
        }
    }
    if (has("vlist-r") && p != kNoNode && HasClass(Classes(doc_.nodes[p].parent), "mfrac")) {
        inh.align = KAlign::Center; // .mfrac > span > span
    }

    const float em = inh.font_size;
    own = {};
    if (any({ "katex-base", "katex-strut", "mspace", "frac-line", "nulldelimiter", "katex-rule", "overline-line", "underline-line",
              "katex-hline", "katex-hdashline", "vertical-separator", "arraycolsep", "katex-fix", "reflectbox", "cd-vert-arrow", "katex-vbox",
              "katex-thinbox", "llap", "rlap", "clap" })) {
        own.display = KDisplay::InlineBlock;
    }
    if (any({ "katex-overlay", "katex-stretchy", "katex-newline" })) {
        own.display = KDisplay::Block;
    }
    if (any({ "katex-base", "delimcenter", "op-symbol", "accent-body", "katex-rule", "llap", "rlap", "clap", "cd-vert-arrow", "katex-stretchy",
              "hide-tail" })) {
        own.position = KPosition::Relative;
    }
    if (IsAbsolute(i)) {
        own.position = KPosition::Absolute;
        own.display = KDisplay::Block;
    }
    // .vlist > span は位置指定した塊。
    if (HasClass(pc, "vlist") && doc_.nodes[i].name == "span") {
        own.position = KPosition::Relative;
    }
    if (has("nulldelimiter")) {
        own.width = KLen{ 0.12f * em };
    }
    if (any({ "katex-thinbox", "llap", "rlap", "clap" }) || (has("accent-body") && !has("accent-full"))) {
        own.width = KLen{ 0.0f };
    }
    if (any({ "frac-line", "overline-line", "underline-line", "katex-hline", "katex-hdashline" })) {
        own.width = KLen{ 1.0f, true };
        own.min_height = 1.0f;
        own.dashed[2] = has("katex-hdashline");
    }
    if (has("vertical-separator")) {
        own.min_width = 1.0f;
    }
    if (has("katex-stretchy") || has("hide-tail")) {
        own.width = KLen{ 1.0f, true };
    }
    if (has("halfarrow-left") || has("halfarrow-right")) {
        own.width = KLen{ 0.502f, true };
        (has("halfarrow-left") ? own.left : own.right) = KLen{ 0.0f };
    }
    if (has("brace-left") || has("brace-right")) {
        own.width = KLen{ 0.251f, true };
        (has("brace-left") ? own.left : own.right) = KLen{ 0.0f };
    }
    if (has("brace-center")) {
        own.width = KLen{ 0.5f, true };
        own.left = KLen{ 0.25f, true };
    }
    if (has("katex-root") && HasClass(pc, "sqrt")) {
        own.margin_l = 0.2777777778f * em;
        own.margin_r = -0.5555555556f * em;
    }
    if (has("x-arrow-pad")) {
        own.padding_l = own.padding_r = 0.5f * em;
    }
    if (has("cd-arrow-pad")) {
        own.padding_l = 0.27778f * em;
        own.padding_r = 0.55556f * em;
    }
    if (has("boxpad")) {
        own.padding_l = own.padding_r = 0.3f * em;
    }
    if (has("cancel-pad")) {
        own.padding_l = own.padding_r = 0.2f * em;
    }
    if (has("anglpad")) {
        own.padding_l = own.padding_r = 0.03889f * em;
    }
    if (has("fbox") || has("fcolorbox")) {
        own.border_box = true;
        own.border.fill(0.04f * em);
    }
    if (has("cancel-lap")) {
        own.margin_l = own.margin_r = -0.2f * em;
    }
    if (has("katex-sout")) {
        own.border[2] = 0.08f * em;
    }
    if (has("angl")) {
        own.border_box = true;
        own.border[0] = own.border[1] = 0.049f * em;
        own.margin_r = 0.03889f * em;
    }
    own.reflect = has("reflectbox");

    const auto style = doc_.nodes[i].Attr("style");
    if (!style) {
        return result;
    }
    ForEachDeclaration(*style, [&](std::string_view name, std::string_view value) {
        bool ok = true;
        const auto px = [&](float& out) {
            const auto v = ParseLength(value, em);
            ok = v.has_value();
            out = v.value_or(out);
        };
        if (name == "height") {
            float h = 0.0f;
            px(h);
            own.height = h;
        }
        else if (name == "width" || name == "min-width") {
            const auto v = ParseKLen(value, em);
            ok = v.has_value();
            if (v && name == "width") {
                own.width = v;
            }
            else if (v && !v->pct) {
                own.min_width = v->v;
            }
        }
        else if (name == "vertical-align") {
            float v = 0.0f;
            px(v);
            own.vertical_align = v;
        }
        else if (name == "top" || name == "left" || name == "right" || name == "bottom") {
            const auto v = ParseKLen(value, em);
            ok = v.has_value();
            (name == "top" ? own.top : name == "left" ? own.left
                                   : name == "right"  ? own.right
                                                      : own.bottom) = v;
        }
        else if (name == "margin-left") {
            px(own.margin_l);
        }
        else if (name == "margin-right") {
            px(own.margin_r);
        }
        else if (name == "margin") {
            std::array<float, 4> v{};
            size_t n = 0;
            ForEachTopLevel(value, ' ', [&](std::string_view part) {
                if (!Trim(part).empty()) {
                    const auto len = ParseLength(part, em);
                    ok = ok && len.has_value();
                    if (n < v.size()) {
                        v[n] = len.value_or(0.0f);
                    }
                    ++n;
                }
                return true;
            });
            if (ok && n > 0) {
                own.margin_r = n >= 2 ? v[1] : v[0];
                own.margin_l = n >= 4 ? v[3] : own.margin_r;
            }
        }
        else if (name == "padding-left") {
            px(own.padding_l);
        }
        else if (name == "border-bottom-width") {
            px(own.border[2]);
        }
        else if (name == "border-right-width") {
            px(own.border[1]);
        }
        else if (name == "border-top-width") {
            px(own.border[0]);
        }
        else if (name == "border-width") {
            float w = 0.0f;
            px(w);
            own.border.fill(w);
        }
        else if (name == "border-right-style") {
            own.dashed[1] = value == "dashed";
        }
        else if (name == "border-style") {
            own.dashed.fill(value == "dashed");
        }
        else if (name == "position") {
            own.position = value == "absolute" ? KPosition::Absolute : value == "relative" ? KPosition::Relative
                                                                                           : KPosition::Static;
        }
        else if (name == "color" || name == "background-color" || name == "border-color") {
            // 2026-11-04 に本文の色だけで描く (\color を読み捨て、\colorbox の背景も描かない) と決めたが、#412 で指定の色を
            // 優先して描くことにした。\colorbox の背景も、ダークモードで中の文字が読みにくくなっても指定どおり描く。
            // 読めない色はブラウザと同じく無視する (数式ごと描けなくはしない)。
            if (Rgba rgba; ParseColor(value, Rgba{}, rgba)) {
                const uint32_t packed = ToRgb(rgba) << 8 | ColorByte(rgba.a);
                (name == "color" ? inh.color : name == "background-color" ? own.background
                                                                          : own.border_color) = packed;
            }
        }
        else if (name == "text-shadow") {
            std::array<float, 2> v{};
            size_t n = 0;
            ForEachTopLevel(value, ' ', [&](std::string_view part) {
                if (const auto len = ParseLength(part, em)) {
                    v[n++] = *len;
                }
                return n < v.size();
            });
            if (n == v.size()) {
                inh.shadow = std::pair{ v[0], v[1] };
            }
        }
        else {
            ok = false;
        }
        if (!ok) {
            Fail();
        }
        return true;
    });
    return result;
}

void KatexLayout::Text(std::string_view text, const KInherit& inh, KBox& out)
{
    const auto [file, sim] = MatchKatexFace(inh.font);
    IDWriteFontFace* primary = assets_.Face(file, sim);
    if (!primary) {
        Fail();
        return;
    }
    DWRITE_FONT_METRICS pm{};
    primary->GetMetrics(&pm);
    const float size = inh.font_size;
    out.asc = std::max(out.asc, pm.ascent * size / pm.designUnitsPerEm);
    out.desc = std::max(out.desc, pm.descent * size / pm.designUnitsPerEm);
    MathDrawing::Glyphs* run = nullptr;
    float x = out.w;
    bool prev_space = false;
    ForEachCp(text, [&](uint32_t cp) {
        if (cp == 0x200B) {
            return;
        }
        // white-space: nowrap は続く空白を 1 つにまとめる。
        const bool space = cp == ' ' || cp == '\t' || cp == '\n';
        if (space && prev_space) {
            return;
        }
        prev_space = space;
        cp = space ? ' ' : cp;
        IDWriteFontFace* face = primary;
        UINT16 glyph = 0;
        face->GetGlyphIndices(&cp, 1, &glyph);
        if (glyph == 0 && cp == 0xA0) {
            const UINT32 sp = ' ';
            face->GetGlyphIndices(&sp, 1, &glyph);
        }
        if (glyph == 0) {
            if (IDWriteFontFace* fb = assets_.Fallback(cp, inh.font.bold, inh.font.italic)) {
                UINT16 g = 0;
                fb->GetGlyphIndices(&cp, 1, &g);
                if (g != 0) {
                    face = fb;
                    glyph = g;
                }
            }
        }
        DWRITE_FONT_METRICS fm = pm;
        if (face != primary) {
            face->GetMetrics(&fm);
        }
        DWRITE_GLYPH_METRICS gm{};
        face->GetDesignGlyphMetrics(&glyph, 1, &gm, FALSE);
        const float advance = gm.advanceWidth * size / fm.designUnitsPerEm;
        if (!run || run->face.Get() != face) {
            run = &std::get<MathDrawing::Glyphs>(out.items.emplace_back(MathDrawing::Glyphs{ .face = face, .size = size, .x = x, .color = inh.color }));
        }
        run->glyphs.push_back(glyph);
        run->advances.push_back(advance);
        x += advance;
    });
    if (inh.shadow) {
        // \pmb は少しずらした写しで太く見せる。写しは元の後ろに足す (CSS の影は先に描くが、同じ色なので重ねる順で見た目は変わらない)。
        const auto [sx, sy] = *inh.shadow;
        for (size_t k = 0, n = out.items.size(); k < n; ++k) {
            if (const auto* g = std::get_if<MathDrawing::Glyphs>(&out.items[k]); g && g->x >= out.w) {
                auto copy = *g;
                copy.x += sx;
                copy.y += sy;
                out.items.emplace_back(std::move(copy));
            }
        }
    }
    out.w = x;
    out.has_content = true;
}

void KatexLayout::Svg(uint32_t i, const KInherit& inh, float x0, float y0, float w, std::optional<float> h, KBox& out)
{
    const auto& n = doc_.nodes[i];
    const float em = inh.font_size;
    if (!h) {
        h = AttrLength(n, "height", em);
    }
    // viewBox が無ければ利用者の座標は CSS px。
    Mat m;
    if (!h || !ViewportTransform(n, x0, y0, w, *h, m)) {
        Fail();
        return;
    }
    const std::array<float, 4> clip{ x0, y0, x0 + w, y0 + *h };
    for (const uint32_t c : n.children) {
        const auto& child = doc_.nodes[c];
        if (child.name == "path") {
            const auto d = child.Attr("d");
            if (!d) {
                Fail();
                return;
            }
            out.items.emplace_back(MathDrawing::Path{ .shape = assets_.Shape(*d), .transform = ToArray(m), .clip = clip, .color = inh.color });
        }
        else if (child.name == "line") {
            // 座標の % はビューポートに対して。viewBox の無い SVG だけで使われる。
            const auto coord = [&](std::string_view attr, float full) {
                const auto a = child.Attr(attr);
                const auto v = a ? ParseKLen(*a, em) : std::optional{ KLen{} };
                return !v ? 0.0f : v->pct ? v->v * full
                                          : v->v;
            };
            const float x1 = coord("x1", w), y1 = coord("y1", *h), x2 = coord("x2", w), y2 = coord("y2", *h);
            const float stroke = AttrLength(child, "stroke-width", em).value_or(1.0f);
            std::string d = "M";
            AppendNum(d, x1);
            d += ' ';
            AppendNum(d, y1);
            d += 'L';
            AppendNum(d, x2);
            d += ' ';
            AppendNum(d, y2);
            out.items.emplace_back(
                MathDrawing::Path{
                    .shape = assets_.Shape(d), .transform = { 1.0f, 0.0f, 0.0f, 1.0f, x0, y0 },
                         .clip = clip, .stroke = stroke, .color = inh.color
            });
        }
        else {
            Fail();
            return;
        }
    }
}

KBox KatexLayout::VList(uint32_t i, const KInherit& inh)
{
    KBox out;
    const auto& rows = doc_.nodes[i].children;
    if (rows.empty()) {
        return out;
    }
    struct Row {
        KBox box;
        float ml = 0.0f, mr = 0.0f, y = 0.0f;
        uint32_t content = kNoNode;
        KInherit wrap_inh;
    };
    std::vector<Row> placed;
    for (size_t r = 0; r < rows.size(); ++r) {
        const uint32_t row = rows[r];
        const KInherit row_inh = Compute(row, inh).inh;
        for (const uint32_t cell : doc_.nodes[row].children) {
            if (HasClass(Classes(cell), "vlist-s")) {
                continue;
            }
            const auto [cell_inh, cell_own] = Compute(cell, row_inh);
            const float h = cell_own.height.value_or(0.0f);
            if (r == 0) {
                out.asc = h;
            }
            else {
                out.desc = h;
                continue;
            }
            for (const uint32_t wrap : doc_.nodes[cell].children) {
                const auto& wn = doc_.nodes[wrap];
                if (wn.children.empty()) {
                    continue;
                }
                Row placed_row;
                const auto [wrap_inh, wrap_own] = Compute(wrap, cell_inh);
                placed_row.wrap_inh = wrap_inh;
                float pstrut = 0.0f;
                for (const uint32_t c : wn.children) {
                    if (HasClass(Classes(c), "pstrut")) {
                        pstrut = Compute(c, placed_row.wrap_inh).own.height.value_or(0.0f);
                    }
                    else if (placed_row.content == kNoNode) {
                        placed_row.content = c;
                    }
                    else {
                        Fail();
                    }
                }
                if (placed_row.content == kNoNode) {
                    continue;
                }
                const float top = wrap_own.top && !wrap_own.top->pct ? wrap_own.top->v : 0.0f;
                placed_row.y = top + pstrut;
                placed_row.ml = wrap_own.margin_l;
                placed_row.mr = wrap_own.margin_r;
                placed_row.box = Element(placed_row.content, placed_row.wrap_inh, KCtx{}, true);
                placed.push_back(std::move(placed_row));
            }
        }
    }
    float col = 0.0f;
    for (const auto& row : placed) {
        col = std::max(col, row.ml + row.box.w + row.mr);
    }
    for (auto& row : placed) {
        const float avail = col - row.ml - row.mr;
        if (row.box.pct) {
            row.box = Element(row.content, row.wrap_inh, KCtx{ avail, avail }, true);
        }
        const float x = row.ml + AlignOffset(row.wrap_inh.align, avail - row.box.w);
        AppendItems(out, std::move(row.box), x, row.y);
    }
    out.w = col;
    out.has_content = true;
    return out;
}

void KatexLayout::Flow(uint32_t i, const KInherit& inh, const KCtx& ctx, KBox& out, std::vector<uint32_t>& absolutes)
{
    for (const uint32_t c : doc_.nodes[i].children) {
        const auto& n = doc_.nodes[c];
        if (n.name.empty()) {
            Text(n.text, inh, out);
            continue;
        }
        if (IsAbsolute(c)) {
            absolutes.push_back(c);
            continue;
        }
        AppendInline(out, Element(c, inh, ctx));
    }
}

KBox KatexLayout::Element(uint32_t i, const KInherit& parent, const KCtx& ctx, bool force_inline_block)
{
    const auto& n = doc_.nodes[i];
    if (n.name != "span") {
        Fail();
        return {};
    }
    auto [inh, own] = Compute(i, parent);
    const auto c = Classes(i);
    // .vlist > span > span の inline-block は .katex-stretchy の block にも勝つ。
    if (force_inline_block) {
        own.display = KDisplay::InlineBlock;
    }
    const bool vlist = HasClass(c, "vlist-t");
    if (HasClass(c, "katex-newline")) {
        Fail();
        return {};
    }

    // 幅 (inline-block と block)。
    std::optional<float> width;
    bool pct = false;
    if (own.display != KDisplay::Inline && own.width) {
        if (!own.width->pct) {
            width = own.width->v;
        }
        else if (ctx.cb_w) {
            width = own.width->v * *ctx.cb_w;
        }
        else {
            pct = true;
        }
    }
    const float bl = own.border[3], br = own.border[1], bt = own.border[0], bb = own.border[2];
    const float edge_l = own.margin_l + bl + own.padding_l;
    const float edge_r = own.padding_r + br + own.margin_r;
    const float frame_w = bl + br + own.padding_l + own.padding_r; // 枠と詰め物の横幅
    std::optional<float> content_w = width;
    if (content_w && own.border_box) {
        content_w = std::max(0.0f, *content_w - frame_w);
    }

    KBox content;
    std::vector<uint32_t> absolutes;
    if (vlist) {
        content = VList(i, inh);
    }
    else {
        if (const uint32_t eqn = eqn_number_[i]) {
            Text("(" + std::to_string(eqn) + ")", inh, content);
        }
        KCtx inner{ content_w, ctx.pos_cb_w };
        Flow(i, inh, inner, content, absolutes);
    }
    KBox out;
    out.pct = pct || (own.display == KDisplay::Inline && content.pct);
    using Side = MathDrawing::Rect::Side;
    const MathDrawing::Color border_color = own.border_color ? own.border_color : inh.color;

    float cw = content.w;
    float top = 0.0f, bottom = 0.0f; // 内容の箱 (ベースラインからの y)
    if (own.display == KDisplay::Inline) {
        out.w = edge_l + content.w + edge_r;
        out.asc = content.asc;
        out.desc = content.desc;
        out.has_content = content.has_content || out.w != 0.0f;
        top = -content.asc;
        bottom = content.desc;
        const float outer_w = content.w + frame_w;
        if (own.background) {
            out.items.emplace_back(MathDrawing::Rect{ own.margin_l, top, outer_w, bottom - top + bb, Side::Fill, false, own.background });
        }
        if (bb > 0.0f) {
            out.items.emplace_back(MathDrawing::Rect{ own.margin_l, bottom, outer_w, bb, Side::Bottom, own.dashed[2], border_color });
        }
        AppendItems(out, std::move(content), edge_l, 0.0f);
    }
    else {
        cw = content_w.value_or(content.w);
        if (own.min_width) {
            cw = std::max(cw, *own.min_width - (own.border_box ? frame_w : 0.0f));
        }
        const bool flow_content = content.has_content || vlist;
        float h = 0.0f;
        if (own.height) {
            h = own.border_box ? std::max(0.0f, *own.height - bt - bb) : *own.height;
        }
        else if (flow_content) {
            h = content.asc + content.desc;
        }
        h = std::max(h, own.min_height);
        if (flow_content) {
            top = -content.asc;
            bottom = top + h;
        }
        else {
            bottom = -bb;
            top = bottom - h;
        }
        float va = own.vertical_align.value_or(0.0f);
        out.w = own.margin_l + bl + own.padding_l + cw + own.padding_r + br + own.margin_r;
        out.asc = -(top - bt) + va;
        out.desc = bottom + bb - va;
        out.has_content = true;
        top -= va;
        bottom -= va;
        const float pad_l = own.margin_l + bl;
        const float pad_w = own.padding_l + cw + own.padding_r;
        const float outer_top = top - bt;
        const float outer_w = bl + pad_w + br;
        const float outer_h = bt + (bottom - top) + bb;
        // 背景は枠の下まで塗る (background-clip: border-box)。
        if (own.background) {
            out.items.emplace_back(MathDrawing::Rect{ own.margin_l, outer_top, outer_w, outer_h, Side::Fill, false, own.background });
        }
        // 枠は外側の端に合わせる (MathDrawing::Rect の rule)。
        if (bt > 0.0f) {
            out.items.emplace_back(MathDrawing::Rect{ own.margin_l, outer_top, outer_w, bt, Side::Top, own.dashed[0], border_color });
        }
        if (bb > 0.0f) {
            out.items.emplace_back(MathDrawing::Rect{ own.margin_l, bottom, outer_w, bb, Side::Bottom, own.dashed[2], border_color });
        }
        if (bl > 0.0f) {
            out.items.emplace_back(MathDrawing::Rect{ own.margin_l, outer_top, bl, outer_h, Side::Left, own.dashed[3], border_color });
        }
        if (br > 0.0f) {
            out.items.emplace_back(MathDrawing::Rect{ pad_l + pad_w, outer_top, br, outer_h, Side::Right, own.dashed[1], border_color });
        }
        AppendItems(out, std::move(content), edge_l + AlignOffset(inh.align, cw - content.w), -va);
    }

    // 絶対位置の子。位置指定した要素なら自分の詰め物の箱、そうでなければ祖先の箱の幅を基準に、自分の内容の左上から置く。
    const bool positioned = own.position != KPosition::Static;
    const float cb_x = own.margin_l + bl;
    const float cb_w = own.padding_l + cw + own.padding_r;
    for (const uint32_t a : absolutes) {
        const auto& an = doc_.nodes[a];
        const std::optional<float> abs_cb = positioned ? std::optional{ cb_w } : ctx.pos_cb_w;
        if (an.name == "svg") {
            const auto [si, so] = Compute(a, inh);
            float w = so.width && !so.width->pct ? so.width->v : abs_cb.value_or(0.0f) * (so.width ? so.width->v : 1.0f);
            if (!abs_cb && !(so.width && !so.width->pct)) {
                out.pct = true;
            }
            // height: inherit。親の高さが auto なら属性の高さ。
            const std::optional<float> h = own.height ? std::optional{ own.border_box ? *own.height - bt - bb : *own.height } : std::nullopt;
            Svg(a, si, positioned ? cb_x : edge_l, top, w, h, out);
            continue;
        }
        const KOwn ao = Compute(a, inh).own;
        const auto resolve = [&](const std::optional<KLen>& v) -> std::optional<float> {
            if (!v) {
                return std::nullopt;
            }
            return v->pct ? std::optional{ v->v * abs_cb.value_or(0.0f) } : std::optional{ v->v };
        };
        const bool inner = HasClass(Classes(a), "katex-inner");
        KCtx actx{ abs_cb, abs_cb };
        if (ao.width && ao.width->pct && !abs_cb) {
            out.pct = true;
        }
        KBox box = Element(a, inh, actx, !inner);
        if (inner) {
            // \rlap などの中身。ベースラインをそろえ、llap は右端、clap は中央を 0 に置く。
            float x = edge_l;
            if (HasClass(c, "llap")) {
                x -= box.w;
            }
            else if (HasClass(c, "clap")) {
                x -= box.w / 2;
            }
            AppendItems(out, std::move(box), x, 0.0f);
            continue;
        }
        float x = cb_x;
        if (const auto left = resolve(ao.left)) {
            x = cb_x + *left;
        }
        else if (const auto right = resolve(ao.right)) {
            x = cb_x + cb_w - box.w - *right;
        }
        float y = top + box.asc; // 箱の上端を内容の上端に
        if (const auto b = resolve(ao.bottom)) {
            y = bottom - box.desc - *b;
        }
        AppendItems(out, std::move(box), x, y);
    }

    // 相対位置。箱は動かさず、描く物だけをずらす。
    if (own.position == KPosition::Relative) {
        float dx = 0.0f, dy = 0.0f;
        if (own.left && !own.left->pct) {
            dx = own.left->v;
        }
        else if (own.right && !own.right->pct) {
            dx = -own.right->v;
        }
        if (own.top && !own.top->pct) {
            dy = own.top->v;
        }
        else if (own.bottom && !own.bottom->pct) {
            dy = -own.bottom->v;
        }
        TranslateItems(out.items, dx, dy);
    }
    if (own.reflect) {
        for (auto& item : out.items) {
            if (auto* g = std::get_if<MathDrawing::Glyphs>(&item)) {
                g->x = out.w - g->x - RunWidth(*g);
                g->mirror = !g->mirror;
            }
        }
    }
    return out;
}

std::optional<MathDrawing> KatexLayout::Run(float font_size)
{
    uint32_t html = kNoNode;
    for (uint32_t i = 0; i < doc_.nodes.size(); ++i) {
        if (HasClass(Classes(i), "katex-html")) {
            html = i;
            break;
        }
    }
    if (html == kNoNode) {
        return std::nullopt;
    }
    KInherit root;
    root.font_size = font_size * 1.21f;
    KBox body;
    std::vector<uint32_t> tags;
    for (const uint32_t c : doc_.nodes[html].children) {
        if (HasClass(Classes(c), "katex-tag")) {
            tags.push_back(c);
            continue;
        }
        if (doc_.nodes[c].name.empty()) {
            Text(doc_.nodes[c].text, root, body);
            continue;
        }
        AppendInline(body, Element(c, root, KCtx{}));
    }
    // 式番号は本体の右に 1em 空けて置く。
    for (const uint32_t t : tags) {
        AppendInline(body, Element(t, root, KCtx{}), root.font_size);
    }
    if (failed_) {
        return std::nullopt;
    }
    MergeGlyphRuns(body.items);
    return MathDrawing{ .width = body.w, .height = body.asc, .depth = body.desc, .items = std::move(body.items) };
}

} // namespace

struct KatexAssets::Impl {
    ComPtr<IDWriteFactory5> dw;
    ComPtr<IDWriteInMemoryFontFileLoader> loader;
    std::array<ComPtr<IDWriteFontFile>, std::size(kKatexFontFiles)> files;
    std::wstring locale;
    std::mutex mutex;
    std::array<std::array<ComPtr<IDWriteFontFace>, 4>, std::size(kKatexFontFiles)> faces;
    std::unordered_map<uint64_t, ComPtr<IDWriteFontFace>> fallback_faces;
    // KaTeX の path は決まった形 (根号・矢印・括弧) の組み合わせなので、種類は数十に収まる。
    std::unordered_map<std::string, std::shared_ptr<const MathDrawing::Shape>, StringTransparentHash, std::equal_to<>> shapes;

    ~Impl()
    {
        if (dw && loader) {
            dw->UnregisterFontFileLoader(loader.Get());
        }
    }
};

KatexAssets::KatexAssets(IDWriteFactory* dw, std::span<const std::span<const std::byte>> woff2, std::wstring locale) : impl_(std::make_unique<Impl>())
{
    auto& m = *impl_;
    m.locale = std::move(locale);
    if (!dw || woff2.size() != std::size(kKatexFontFiles) || FAILED(dw->QueryInterface(IID_PPV_ARGS(&m.dw))) ||
        FAILED(m.dw->CreateInMemoryFontFileLoader(&m.loader))) {
        return;
    }
    if (FAILED(m.dw->RegisterFontFileLoader(m.loader.Get()))) {
        m.loader.Reset();
        return;
    }
    for (size_t i = 0; i < woff2.size(); ++i) {
        ComPtr<IDWriteFontFileStream> stream;
        UINT64 size = 0;
        const void* data = nullptr;
        void* context = nullptr;
        if (FAILED(m.dw->UnpackFontFile(DWRITE_CONTAINER_TYPE_WOFF2, woff2[i].data(), static_cast<UINT32>(woff2[i].size()), &stream)) ||
            FAILED(stream->GetFileSize(&size)) || FAILED(stream->ReadFileFragment(&data, 0, size, &context))) {
            return;
        }
        // 持ち主を渡さないと DirectWrite が写しを持つ。
        const HRESULT hr = m.loader->CreateInMemoryFontFileReference(m.dw.Get(), data, static_cast<UINT32>(size), nullptr, &m.files[i]);
        stream->ReleaseFileFragment(context);
        if (FAILED(hr)) {
            return;
        }
    }
    ok_ = true;
}

KatexAssets::~KatexAssets() = default;

IDWriteFontFace* KatexAssets::Face(size_t file, uint32_t simulations)
{
    auto& m = *impl_;
    if (!ok_ || file >= m.files.size() || simulations > 3) {
        return nullptr;
    }
    const std::lock_guard lock(m.mutex);
    auto& face = m.faces[file][simulations];
    if (!face) {
        m.dw->CreateFontFace(
            DWRITE_FONT_FACE_TYPE_TRUETYPE,
            1,
            m.files[file].GetAddressOf(),
            0,
            static_cast<DWRITE_FONT_SIMULATIONS>(simulations),
            &face);
    }
    return face.Get();
}

IDWriteFontFace* KatexAssets::Fallback(uint32_t cp, bool bold, bool italic)
{
    auto& m = *impl_;
    if (!ok_) {
        return nullptr;
    }
    const uint64_t key = (static_cast<uint64_t>(cp) << 2) | (bold ? 1u : 0u) | (italic ? 2u : 0u);
    const std::lock_guard lock(m.mutex);
    if (const auto it = m.fallback_faces.find(key); it != m.fallback_faces.end()) {
        return it->second.Get();
    }
    ComPtr<IDWriteFontFace> face;
    if (const auto font = MapSystemFallback(m.dw.Get(), cp, kDefaultFamily.data(), bold ? DWRITE_FONT_WEIGHT_BOLD : DWRITE_FONT_WEIGHT_NORMAL,
                                            italic ? DWRITE_FONT_STYLE_ITALIC : DWRITE_FONT_STYLE_NORMAL, m.locale.c_str())) {
        font->CreateFontFace(&face);
    }
    return m.fallback_faces.emplace(key, std::move(face)).first->second.Get();
}

std::shared_ptr<const MathDrawing::Shape> KatexAssets::Shape(std::string_view d)
{
    auto& m = *impl_;
    const std::lock_guard lock(m.mutex);
    if (const auto it = m.shapes.find(d); it != m.shapes.end()) {
        return it->second;
    }
    auto shape = std::make_shared<const MathDrawing::Shape>(MathDrawing::Shape{ .d = std::string{ d } });
    m.shapes.emplace(shape->d, shape);
    return shape;
}

std::optional<MathDrawing> LayoutKatexHtml(std::string_view html, float font_size, KatexAssets& assets)
{
    XmlDoc doc;
    if (!assets.ok() || !ParseXml(html, doc)) {
        return std::nullopt;
    }
    return KatexLayout(doc, assets).Run(font_size);
}

namespace {

// 形を塗る。box_to_target は SVG の箱の座標から描く先への変換 (拡大と平行移動だけ) で、clip は箱の座標。
// マスクも本体もこれで塗り、画素をそろえる。
void PaintPath(ID2D1RenderTarget* rt, ID2D1PathGeometry* geometry, const std::array<float, 6>& transform, const D2D1_RECT_F& clip, float stroke,
               const D2D1::Matrix3x2F& box_to_target, ID2D1Brush* brush)
{
    rt->SetTransform(box_to_target);
    rt->PushAxisAlignedClip(clip, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
    rt->SetTransform(ToD2D(transform) * box_to_target);
    if (stroke > 0.0f) {
        rt->DrawGeometry(geometry, brush, stroke);
    }
    else {
        rt->FillGeometry(geometry, brush);
    }
    rt->SetTransform(box_to_target);
    rt->PopAxisAlignedClip();
}

} // namespace

struct MathMaskCache::Impl {
    // 箱の左上を原点にした形。数式の中の箱の位置は含めないので、同じ形を同じ大きさで使う数式の間で共有する。
    struct Key {
        // 数式を捨てると形も消えうる。アドレスが別の形に使い回されないよう、キーが持つ。
        std::shared_ptr<const MathDrawing::Shape> shape;
        std::array<float, 6> transform{};
        float width = 0.0f;
        float height = 0.0f;
        float stroke = 0.0f;
        float scale_x = 0.0f;
        float scale_y = 0.0f;

        bool operator==(const Key&) const = default;
    };
    struct Entry {
        Key key;
        // 作れなかった形は null のまま残し、毎フレーム作り直さない (呼び出し側が形を塗る)。
        ComPtr<ID2D1Bitmap1> bitmap;
        UINT width = 0;
        UINT height = 0;
        uint64_t last_frame = 0;

        // 小さな A8 の描画先でも、1 枚で private が約 14KB 増えた (2000 枚で計測)。
        size_t Bytes() const noexcept
        {
            return bitmap ? std::max<size_t>(static_cast<size_t>(width) * height, 14 * 1024) : 0;
        }
    };

    ComPtr<ID2D1Device> device;
    // マスクを描く。本体の context は描いている最中で、BeginDraw は入れ子にできないので別に持つ。
    ComPtr<ID2D1DeviceContext> dc;
    ComPtr<ID2D1SolidColorBrush> opaque;
    std::unordered_map<uint64_t, Entry> entries;
    size_t bytes = 0;
    uint64_t frame = 0;

    // Key は shared_ptr を含みバイト列のまま渡せないので、形のアドレスを seed にして値の部分だけを並べる。
    static uint64_t Hash(const Key& key) noexcept
    {
        const auto& t = key.transform;
        const std::array<float, 11> v{ t[0], t[1], t[2], t[3], t[4], t[5], key.width, key.height, key.stroke, key.scale_x, key.scale_y };
        return rapidhash_withSeed(v.data(), sizeof(v), std::bit_cast<uintptr_t>(key.shape.get()));
    }

    bool EnsureContext()
    {
        if (dc) {
            return true;
        }
        if (!device || FAILED(device->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &dc)) ||
            FAILED(dc->CreateSolidColorBrush(D2D1::ColorF(1.0f, 1.0f, 1.0f, 1.0f), &opaque))) {
            dc.Reset();
            return false;
        }
        dc->SetDpi(DEFAULT_DPI, DEFAULT_DPI);
        return true;
    }

    // 装置の画素を 1 単位として形を塗る。
    Entry Make(const Key& key, ID2D1PathGeometry* geometry)
    {
        Entry e{ .key = key,
                 .width = static_cast<UINT>(std::ceil(key.width * key.scale_x)),
                 .height = static_cast<UINT>(std::ceil(key.height * key.scale_y)) };
        if (e.width == 0 || e.height == 0 || !EnsureContext() || std::max(e.width, e.height) > dc->GetMaximumBitmapSize()) {
            return e;
        }
        const auto props = D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_TARGET, D2D1::PixelFormat(DXGI_FORMAT_A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
        ComPtr<ID2D1Bitmap1> bitmap;
        if (FAILED(dc->CreateBitmap(D2D1::SizeU(e.width, e.height), nullptr, 0, &props, &bitmap))) {
            return e;
        }
        dc->SetTarget(bitmap.Get());
        dc->BeginDraw();
        dc->Clear(D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.0f));
        PaintPath(dc.Get(), geometry, key.transform, D2D1::RectF(0.0f, 0.0f, key.width, key.height), key.stroke,
                  D2D1::Matrix3x2F::Scale(key.scale_x, key.scale_y), opaque.Get());
        const HRESULT hr = dc->EndDraw();
        dc->SetTarget(nullptr);
        if (SUCCEEDED(hr)) {
            e.bitmap = std::move(bitmap);
        }
        return e;
    }
};

MathMaskCache::MathMaskCache() : impl_(std::make_unique<Impl>())
{}
MathMaskCache::~MathMaskCache() = default;

void MathMaskCache::Reset(ID2D1Device* device) noexcept
{
    auto& m = *impl_;
    m.entries.clear();
    m.bytes = 0;
    m.dc.Reset();
    m.opaque.Reset();
    m.device = device;
}

void MathMaskCache::EndFrame(size_t budget_bytes)
{
    auto& m = *impl_;
    if (m.bytes > budget_bytes) {
        std::vector<std::pair<uint64_t, uint64_t>> stale;
        for (const auto& [hash, e] : m.entries) {
            if (e.last_frame != m.frame) {
                stale.emplace_back(e.last_frame, hash);
            }
        }
        std::ranges::sort(stale);
        for (const auto& [frame, hash] : stale) {
            if (m.bytes <= budget_bytes) {
                break;
            }
            const auto it = m.entries.find(hash);
            m.bytes -= it->second.Bytes();
            m.entries.erase(it);
        }
    }
    ++m.frame;
}

bool MathMaskCache::Fill(ID2D1RenderTarget* rt, const MathDrawing::Path& path, ID2D1PathGeometry* geometry, float left, float top, float scale_x,
                         float scale_y, ID2D1Brush* brush)
{
    auto& m = *impl_;
    if (!m.device || !(scale_x > 0.0f) || !(scale_y > 0.0f)) {
        return false;
    }
    const auto& t = path.transform;
    const auto& c = path.clip;
    const Impl::Key key{
        .shape = path.shape,
        .transform = { t[0], t[1], t[2], t[3], t[4] - c[0], t[5] - c[1] },
        .width = c[2] - c[0],
        .height = c[3] - c[1],
        .stroke = path.stroke,
        .scale_x = scale_x,
        .scale_y = scale_y,
    };
    auto& entry = m.entries[Impl::Hash(key)];
    if (entry.key != key) {
        m.bytes -= entry.Bytes();
        entry = m.Make(key, geometry);
        m.bytes += entry.Bytes();
    }
    entry.last_frame = m.frame;
    if (!entry.bitmap) {
        return false;
    }
    const auto w = static_cast<float>(entry.width);
    const auto h = static_cast<float>(entry.height);
    const D2D1_RECT_F dest = D2D1::RectF(left, top, left + w / scale_x, top + h / scale_y);
    const D2D1_RECT_F src = D2D1::RectF(0.0f, 0.0f, w, h);
    // FillOpacityMask は ALIASED のときしか描かない。
    const D2D1_ANTIALIAS_MODE mode = rt->GetAntialiasMode();
    rt->SetAntialiasMode(D2D1_ANTIALIAS_MODE_ALIASED);
    rt->FillOpacityMask(entry.bitmap.Get(), brush, D2D1_OPACITY_MASK_CONTENT_GRAPHICS, &dest, &src);
    rt->SetAntialiasMode(mode);
    return true;
}

size_t MathMaskCache::Count() const noexcept
{
    return impl_->entries.size();
}

size_t MathMaskCache::Bytes() const noexcept
{
    return impl_->bytes;
}

namespace {

// 色の指定の無いグリフを、書体と大きさごとに 1 つの run にまとめ、ベースラインのずれは glyphOffsets で付ける。DrawGlyphRun は
// 1 回ごとの手間が大きく、形の多い数式 (1 式 7.5 run) の画面で 1 割ほど縮む。画素は 1 つずつ描くのと同じ。
// 同じブラシで塗る分どうしは描く順で画素が変わらないので、まとめた run は色を指定した部分の手前か数式の最後に描く。
class GlyphRunMerger {
public:
    void Add(const MathDrawing::Glyphs& g, float x, float baseline)
    {
        if (g.glyphs.empty()) {
            return;
        }
        const auto used = std::span{ groups_ }.first(used_);
        const auto it = std::ranges::find_if(used, [&](const Group& grp) { return grp.face == g.face.Get() && grp.size == g.size; });
        Group* grp = it != used.end() ? std::to_address(it) : nullptr;
        if (!grp) {
            if (used_ == groups_.size()) {
                groups_.emplace_back();
            }
            grp = &groups_[used_++];
            grp->face = g.face.Get();
            grp->size = g.size;
            grp->origin = D2D1::Point2F(x, baseline);
            grp->glyphs.clear();
            grp->advances.clear();
            grp->offsets.clear();
        }
        // 前の run の最後のグリフの送りで、ペンをこの run の先頭まで進める。
        if (!grp->advances.empty()) {
            grp->advances.back() += x - grp->pen;
        }
        grp->glyphs.append_range(g.glyphs);
        grp->advances.append_range(g.advances);
        grp->offsets.insert(grp->offsets.end(), g.glyphs.size(), { .advanceOffset = 0.0f, .ascenderOffset = grp->origin.y - baseline });
        grp->pen = std::ranges::fold_left(g.advances, x, std::plus{});
    }

    void Flush(ID2D1RenderTarget* rt, ID2D1Brush* brush)
    {
        for (const Group& grp : std::span{ groups_ }.first(used_)) {
            const DWRITE_GLYPH_RUN run{
                .fontFace = grp.face,
                .fontEmSize = grp.size,
                .glyphCount = static_cast<UINT32>(grp.glyphs.size()),
                .glyphIndices = grp.glyphs.data(),
                .glyphAdvances = grp.advances.data(),
                .glyphOffsets = grp.offsets.data(),
            };
            rt->DrawGlyphRun(grp.origin, &run, brush, DWRITE_MEASURING_MODE_NATURAL);
        }
        used_ = 0;
    }

private:
    struct Group {
        IDWriteFontFace* face = nullptr;
        float size = 0.0f;
        D2D1_POINT_2F origin{};
        float pen = 0.0f;
        std::vector<uint16_t> glyphs;
        std::vector<float> advances;
        std::vector<DWRITE_GLYPH_OFFSET> offsets;
    };
    // 毎フレーム確保し直さないよう、配列を残して使い回す。
    std::vector<Group> groups_;
    size_t used_ = 0;
};

} // namespace

void DrawMath(ID2D1RenderTarget* rt, const MathDrawing& math, float x, float y, ID2D1Brush* brush, MathMaskCache* masks)
{
    D2D1::Matrix3x2F base;
    rt->GetTransform(&base);
    ComPtr<ID2D1Factory> factory;
    rt->GetFactory(&factory);
    // 装置の画素に合わせる。描く先の変換は拡大と平行移動だけ (図とインライン数式の縮小) とみなす。
    float dpi_x = DEFAULT_DPI, dpi_y = DEFAULT_DPI;
    rt->GetDpi(&dpi_x, &dpi_y);
    const float scale_x = base._11 * dpi_x / DEFAULT_DPI, scale_y = base._22 * dpi_y / DEFAULT_DPI;
    const float offset_x = base._31 * dpi_x / DEFAULT_DPI, offset_y = base._32 * dpi_y / DEFAULT_DPI;
    const auto snap_x = [&](float v) { return scale_x > 0.0f ? (std::round(v * scale_x + offset_x) - offset_x) / scale_x : v; };
    const auto snap_y = [&](float v) { return scale_y > 0.0f ? (std::round(v * scale_y + offset_y) - offset_y) / scale_y : v; };
    // 罫線の太さ。Chrome の border と同じく CSS px に切り捨て、1px 未満は 1px にする (分数の線 0.04em は DPR 1.5 で 1.5 画素)。
    const auto thickness = [](float v) { return v < 1.0f ? 1.0f : std::floor(v + 1e-3f); };
    // 色を指定した部分のブラシ。色の無い数式では作らない。
    ComPtr<ID2D1SolidColorBrush> colored;
    const auto brush_for = [&](const MathDrawing::Color& color) -> ID2D1Brush* {
        if (!color) {
            return brush;
        }
        const D2D1::ColorF c(*color >> 8, static_cast<float>(*color & 0xFF) / 255.0f);
        if (!colored && FAILED(rt->CreateSolidColorBrush(c, &colored))) {
            return brush;
        }
        colored->SetColor(c);
        colored->SetOpacity(brush->GetOpacity());
        return colored.Get();
    };
    thread_local GlyphRunMerger merger;
    for (const auto& item : math.items) {
        // 色を指定した部分 (\colorbox の背景など) との重なりの順を保つ。
        if (std::visit([](const auto& it) noexcept { return it.color.has_value(); }, item)) {
            merger.Flush(rt, brush);
        }
        if (const auto* g = std::get_if<MathDrawing::Glyphs>(&item)) {
            // ベースラインは装置の画素に合わせ、横は合わせない (Chrome と同じ。横も合わせると Chrome の絵から離れた)。
            const float gy = snap_y(y + g->y);
            if (!g->color && !g->mirror) {
                merger.Add(*g, x + g->x, gy);
                continue;
            }
            DWRITE_GLYPH_RUN run{};
            run.fontFace = g->face.Get();
            run.fontEmSize = g->size;
            run.glyphCount = static_cast<UINT32>(g->glyphs.size());
            run.glyphIndices = g->glyphs.data();
            run.glyphAdvances = g->advances.data();
            if (g->mirror) {
                rt->SetTransform(D2D1::Matrix3x2F::Scale(-1.0f, 1.0f, D2D1::Point2F(x + g->x + RunWidth(*g) / 2, gy)) * base);
            }
            rt->DrawGlyphRun(D2D1::Point2F(x + g->x, gy), &run, brush_for(g->color), DWRITE_MEASURING_MODE_NATURAL);
            if (g->mirror) {
                rt->SetTransform(base);
            }
        }
        else if (const auto* r = std::get_if<MathDrawing::Rect>(&item)) {
            using Side = MathDrawing::Rect::Side;
            float left = snap_x(x + r->x), right = snap_x(x + r->x + r->w);
            float top = snap_y(y + r->y), bottom = snap_y(y + r->y + r->h);
            switch (r->side) {
            case Side::Top:
                bottom = top + thickness(r->h);
                break;
            case Side::Bottom:
                top = bottom - thickness(r->h);
                break;
            case Side::Left:
                right = left + thickness(r->w);
                break;
            case Side::Right:
                left = right - thickness(r->w);
                break;
            case Side::Fill:
                break;
            }
            if (right <= left || bottom <= top) {
                continue;
            }
            ID2D1Brush* const rect_brush = brush_for(r->color);
            if (!r->dashed) {
                rt->FillRectangle(D2D1::RectF(left, top, right, bottom), rect_brush);
                continue;
            }
            // Chrome の破線に近い、太さの 3 倍の線と隙間。
            const bool horizontal = right - left >= bottom - top;
            const float t = horizontal ? bottom - top : right - left;
            const float len = horizontal ? right - left : bottom - top;
            for (float p = 0.0f; p < len; p += t * 6) {
                const float e = std::min(len, p + t * 3);
                rt->FillRectangle(horizontal ? D2D1::RectF(left + p, top, left + e, bottom) : D2D1::RectF(left, top + p, right, top + e), rect_brush);
            }
        }
        else if (const auto* p = std::get_if<MathDrawing::Path>(&item)) {
            const auto& shape = *p->shape;
            ComPtr<ID2D1PathGeometry> geometry = shape.geometry;
            // 形は作ったファクトリの描画先でしか描けない。本番は本体のファクトリ 1 つで描くので作り直さないが、形は数式を
            // またいで共有するので、別のファクトリで描かれたら付け替える (最初のファクトリに固定すると、後のファクトリが毎フレーム作り直す)。
            if (shape.factory != factory) {
                geometry = BuildPathGeometry(factory.Get(), shape.d);
                if (geometry) {
                    shape.factory = factory;
                    shape.geometry = geometry;
                }
            }
            if (!geometry) {
                continue;
            }
            // SVG の箱は Chrome と同じく左上を装置の画素に合わせる (\xlongequal の線が 0.7 画素ずれた)。
            const float left = snap_x(x + p->clip[0]);
            const float top = snap_y(y + p->clip[1]);
            ID2D1Brush* const path_brush = brush_for(p->color);
            if (masks && masks->Fill(rt, *p, geometry.Get(), left, top, scale_x, scale_y, path_brush)) {
                continue;
            }
            PaintPath(rt, geometry.Get(), p->transform, D2D1::RectF(p->clip[0], p->clip[1], p->clip[2], p->clip[3]), p->stroke,
                      D2D1::Matrix3x2F::Translation(left - p->clip[0], top - p->clip[1]) * base, path_brush);
            rt->SetTransform(base);
        }
    }
    merger.Flush(rt, brush);
}

std::vector<std::wstring> ResolveFontFamilies(IDWriteFontCollection* fonts, std::wstring_view css_family)
{
    std::vector<std::wstring> out;
    const auto add = [&out](std::wstring_view name) {
        if (std::ranges::find(out, name) == out.end()) {
            out.emplace_back(name);
        }
    };
    while (!css_family.empty()) {
        const auto comma = css_family.find(L',');
        const std::wstring_view name = Unquote(Trim(css_family.substr(0, comma)));
        css_family = comma == std::wstring_view::npos ? std::wstring_view{} : css_family.substr(comma + 1);
        if (name.empty()) {
            continue;
        }
        // CSS の総称名は大文字小文字を区別しない。
        if (ascii_util::iequal(name, L"monospace")) {
            const auto mono = std::ranges::find_if(theme_palette::kMonospaceFontCandidates, [&](const wchar_t* f) { return HasFamily(fonts, f); });
            if (mono != std::end(theme_palette::kMonospaceFontCandidates)) {
                add(*mono);
            }
            continue;
        }
        const auto generic = std::ranges::find_if(kGenericFamilies, [name](const auto& g) {
            return std::ranges::equal(name, g.first, {}, ascii_util::ToLowerAscii);
        });
        if (generic != std::end(kGenericFamilies)) {
            add(generic->second);
        }
        else if (std::wstring candidate{ name }; HasFamily(fonts, candidate.c_str())) {
            add(candidate);
        }
    }
    return out;
}

DiagramTextMeasurer::DiagramTextMeasurer(IDWriteFactory* dw, std::wstring locale) : dw_(dw), locale_(std::move(locale))
{}

DiagramTextMeasurer::~DiagramTextMeasurer() = default;

DiagramTextMeasurer::FontEntry& DiagramTextMeasurer::Entry(const DiagramFont& font)
{
    key_.clear();
    AppendFontKey(key_, font);
    auto it = entries_.find(key_);
    if (it == entries_.end()) {
        it = entries_.emplace(key_, FontEntry{ .format = dw_ ? CreateDiagramFormat(dw_.Get(), font, locale_.c_str()) : nullptr }).first;
    }
    return it->second;
}

IDWriteTextFormat* DiagramTextMeasurer::Format(const DiagramFont& font)
{
    return Entry(font).format.Get();
}

float DiagramTextMeasurer::Width(std::wstring_view text, const DiagramFont& font)
{
    // 図の文字は数千種類なので、長く使っても膨らみ続けないよう上限で捨てる。
    constexpr size_t kMaxWidths = 20000;
    key_.clear();
    AppendFontKey(key_, font);
    key_.push_back(L'\0');
    key_.append(text);
    if (const auto it = widths_.find(key_); it != widths_.end()) {
        return it->second;
    }
    std::wstring key = std::move(key_);
    float width = 0.0f;
    ComPtr<IDWriteTextLayout> layout;
    IDWriteTextFormat* format = text.empty() ? nullptr : Format(font);
    if (format && SUCCEEDED(dw_->CreateTextLayout(text.data(), static_cast<UINT32>(text.size()), format, 1.0e6f, 1.0e6f, &layout))) {
        DWRITE_TEXT_METRICS metrics{};
        if (SUCCEEDED(layout->GetMetrics(&metrics))) {
            width = metrics.widthIncludingTrailingWhitespace;
        }
    }
    if (widths_.size() >= kMaxWidths) {
        widths_.clear();
    }
    widths_.emplace(std::move(key), width);
    return width;
}

std::pair<float, float> DiagramTextMeasurer::Extent(const DiagramFont& font)
{
    FontEntry& entry = Entry(font);
    if (!entry.extent) {
        entry.extent = std::pair{ font.size * 0.9f, font.size * 0.3f };
        ComPtr<IDWriteTextLayout> layout;
        DWRITE_LINE_METRICS line{};
        UINT32 lines = 0;
        if (entry.format && SUCCEEDED(dw_->CreateTextLayout(L"x", 1, entry.format.Get(), 1.0e6f, 1.0e6f, &layout)) &&
            SUCCEEDED(layout->GetLineMetrics(&line, 1, &lines)) && lines == 1) {
            entry.extent = std::pair{ line.baseline, line.height - line.baseline };
        }
    }
    return *entry.extent;
}

std::string DrawingToSvg(const mermaid_util::DiagramDrawing& drawing)
{
    std::string out;
    AppendSvgHead(out, drawing);
    // 文字の並びと数式で共通の <text の属性。この後に字体の属性と >本文</text> を続ける。
    const auto open_text = [&out](const mermaid_util::DrawingPlacement& at, std::string_view family) {
        out += "<text";
        AppendTransform(out, at.transform);
        AppendAttr(out, "x", at.x);
        AppendAttr(out, "y", at.y);
        AppendAttr(out, "font-family", family);
        AppendAttr(out, "font-size", at.font_size);
    };
    const auto fill = [&out](const mermaid_util::DrawingPlacement& at) {
        AppendAttr(out, "fill", FromRgb(at.rgb));
        AppendAttrUnlessInitial(out, "fill-opacity", at.alpha, 1.0);
        out += " xml:space=\"preserve\">";
    };
    for (const auto& layer : drawing.layers) {
        out += layer.shapes;
        for (const auto& t : layer.texts) {
            open_text(t.at, string_convert::WideToUtf8(t.family));
            AppendAttrUnlessInitial(out, "font-weight", t.weight, 400.0);
            if (t.italic) {
                out += " font-style=\"italic\"";
            }
            if (t.decoration != 0) {
                constexpr std::string_view kLines[] = { "", "underline", "line-through", "underline line-through" };
                static_assert(std::size(kLines) == mermaid_util::kTextDecorationBits + 1);
                AppendAttr(out, "text-decoration", kLines[t.decoration & mermaid_util::kTextDecorationBits]);
            }
            fill(t.at);
            ascii_util::AppendHtmlEscaped(out, string_convert::WideToUtf8(t.text));
            out += "</text>";
        }
        // 数式は Office や Inkscape で読めるよう、ラベルに書いたとおりの $$TeX$$ にする。書体は総称の読み替えと同じ本文の書体。
        for (const auto& m : layer.maths) {
            open_text(m.at, kDefaultFamilyUtf8);
            fill(m.at);
            out += "$$";
            ascii_util::AppendHtmlEscaped(out, m.tex);
            out += "$$</text>";
        }
    }
    out += "</svg>";
    return out;
}

std::optional<mermaid_util::DiagramDrawing> DrawingFromPeleSvg(
    std::string_view svg, bool dark, float max_width, DiagramTextMeasurer& measurer,
    std::span<const PeleMathLabel> maths)
{
    XmlDoc doc;
    if (!ParseXml(svg, doc)) {
        return std::nullopt;
    }
    return PeleSvgResolver(doc, measurer, maths).Run(dark, max_width);
}

ComPtr<ID2D1DeviceContext5> CreateRecordContext(ID2D1RenderTarget* target)
{
    ComPtr<ID2D1DeviceContext> dc;
    ComPtr<ID2D1Device> device;
    ComPtr<ID2D1DeviceContext> record;
    ComPtr<ID2D1DeviceContext5> out;
    if (target && SUCCEEDED(target->QueryInterface(IID_PPV_ARGS(&dc)))) {
        dc->GetDevice(&device);
        if (device && SUCCEEDED(device->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &record))) {
            record.As(&out);
        }
    }
    return out;
}

namespace {

// dc の今の変換に重ねて、図の左上を原点とする CSS px で、背景から描く。BeginDraw と EndDraw の間で呼ぶ。
// wic は図形の SVG を読むストリームに使う。描けなければ false (描きかけのまま)。
bool DrawDiagram(
    ID2D1DeviceContext5* dc, IDWriteFactory* dw, IWICImagingFactory* wic, const mermaid_util::DiagramDrawing& drawing,
    const wchar_t* locale)
{
    ComPtr<ID2D1SolidColorBrush> brush;
    if (FAILED(dc->CreateSolidColorBrush(D2D1::ColorF(drawing.background), &brush))) {
        return false;
    }
    D2D1::Matrix3x2F base;
    dc->GetTransform(&base);
    // 縮めて描くことがあるので、ClearType の色にじみを持ち込まない。背景はキャプチャと同じく塗る。
    dc->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
    dc->FillRectangle(D2D1::RectF(0.0f, 0.0f, drawing.width, drawing.height), brush.Get());
    DiagramTextMeasurer formats(dw, locale);
    // 層の図形は <svg> の中身だけなので、どの層にも同じ頭と <defs> を付けて 1 枚の SVG にする。
    std::string svg;
    AppendSvgHead(svg, drawing);
    const size_t head_size = svg.size();
    bool ok = true;
    for (const auto& layer : drawing.layers) {
        if (!layer.shapes.empty()) {
            svg.resize(head_size);
            svg += layer.shapes;
            svg += "</svg>";
            const std::span bytes{ reinterpret_cast<const uint8_t*>(svg.data()), svg.size() };
            const auto stream = wic_util::CreateStreamOnMemory(wic, bytes);
            ComPtr<ID2D1SvgDocument> doc;
            if (!stream || FAILED(dc->CreateSvgDocument(stream.Get(), D2D1::SizeF(drawing.width, drawing.height), &doc))) {
                ok = false;
                break;
            }
            dc->SetTransform(base);
            dc->DrawSvgDocument(doc.Get());
        }
        for (const auto& t : layer.texts) {
            DrawRun(dc, dw, formats, brush.Get(), base, t);
        }
        for (const auto& m : layer.maths) {
            if (!m.drawing) {
                continue;
            }
            Place(dc, brush.Get(), base, m.at);
            DrawMath(dc, *m.drawing, m.at.x, m.at.y, brush.Get());
        }
    }
    dc->SetTransform(base);
    return ok;
}

} // namespace

ComPtr<ID2D1CommandList> RecordDiagram(
    ID2D1DeviceContext5* record_dc, IDWriteFactory* dw, IWICImagingFactory* wic,
    const mermaid_util::DiagramDrawing& drawing, const wchar_t* locale)
{
    ComPtr<ID2D1CommandList> commands;
    if (!record_dc || !dw || FAILED(record_dc->CreateCommandList(&commands))) {
        return nullptr;
    }
    record_dc->SetTarget(commands.Get());
    record_dc->BeginDraw();
    record_dc->SetTransform(D2D1::Matrix3x2F::Identity());
    const bool ok = DrawDiagram(record_dc, dw, wic, drawing, locale);
    const HRESULT hr = record_dc->EndDraw();
    record_dc->SetTarget(nullptr);
    if (!ok || FAILED(hr) || FAILED(commands->Close())) {
        return nullptr;
    }
    return commands;
}

Microsoft::WRL::ComPtr<IWICBitmap> RasterizeDiagram(
    ID2D1Factory* d2d, IDWriteFactory* dw, IWICImagingFactory* wic,
    const mermaid_util::DiagramDrawing& drawing, float dpr, const wchar_t* locale)
{
    const auto width = static_cast<UINT>(std::ceil(drawing.width * dpr));
    const auto height = static_cast<UINT>(std::ceil(drawing.height * dpr));
    if (!d2d || !dw || !wic || width == 0 || height == 0) {
        return nullptr;
    }
    ComPtr<IWICBitmap> bitmap;
    if (FAILED(wic->CreateBitmap(width, height, GUID_WICPixelFormat32bppPBGRA, WICBitmapCacheOnLoad, &bitmap))) {
        return nullptr;
    }
    const float dpi = DEFAULT_DPI * dpr;
    const auto props = D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_SOFTWARE,
                                                    D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), dpi, dpi);
    ComPtr<ID2D1RenderTarget> rt;
    ComPtr<ID2D1DeviceContext5> dc;
    if (FAILED(d2d->CreateWicBitmapRenderTarget(bitmap.Get(), props, &rt)) || FAILED(rt.As(&dc))) {
        return nullptr;
    }
    dc->BeginDraw();
    const bool ok = DrawDiagram(dc.Get(), dw, wic, drawing, locale);
    if (FAILED(dc->EndDraw()) || !ok) {
        return nullptr;
    }
    return bitmap;
}
