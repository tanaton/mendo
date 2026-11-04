#include "diagram_raster.h"
#include "ascii_util.h"
#include "string_convert.h"
#include "theme_palette.h"
#include "utf8_codec.h"
#include "wic_util.h"
#include <d2d1_3.h>
#include <dwrite_3.h>
#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <deque>
#include <limits>
#include <mutex>
#include <numbers>
#include <span>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

using namespace std::literals;
using Microsoft::WRL::ComPtr;

namespace {

constexpr std::wstring_view kDefaultFamily = L"Segoe UI"sv;

// 総称は本文 (Theme) と同じ書体に読み替える。等幅は Theme と同じく候補から入っているものを選ぶ。
constexpr std::pair<std::wstring_view, std::wstring_view> kGenericFamilies[]{
    { L"sans-serif"sv, kDefaultFamily },
    { L"system-ui"sv, kDefaultFamily },
    { L"serif"sv, L"Times New Roman"sv },
};

bool HasFamily(IDWriteFontCollection* fonts, const wchar_t* name) noexcept
{
    UINT32 index = 0;
    BOOL exists = FALSE;
    return fonts && SUCCEEDED(fonts->FindFamilyName(name, &index, &exists)) && exists;
}

std::wstring_view TrimFamily(std::wstring_view s) noexcept
{
    constexpr std::wstring_view kSpace = L" \t\r\n"sv;
    const auto first = s.find_first_not_of(kSpace);
    if (first == std::wstring_view::npos) {
        return {};
    }
    s = s.substr(first, s.find_last_not_of(kSpace) - first + 1);
    if (s.size() >= 2 && (s.front() == L'"' || s.front() == L'\'') && s.back() == s.front()) {
        s = s.substr(1, s.size() - 2);
    }
    return s;
}

// font-family の並びごとの、先頭の書体とフォールバック。書式ごとに組み直すと、DirectWrite がフォールバック先を
// 引いた結果も書式ごとに冷えるので、プロセスで共有する。
struct FamilyChain {
    std::wstring primary;
    ComPtr<IDWriteFontFallback> fallback; // 2 番目以降の候補 → システム。候補が 1 つなら null
};

FamilyChain ChainFor(IDWriteFactory* dw, IDWriteFontCollection* fonts, std::wstring_view css_family)
{
    static std::mutex mutex;
    // 終了時に DirectWrite のオブジェクトを解放させない (解放の順が決まらない)。
    static auto* const chains = new std::unordered_map<std::wstring, FamilyChain>();
    const std::lock_guard lock(mutex);
    std::wstring key{ css_family };
    if (const auto it = chains->find(key); it != chains->end()) {
        return it->second;
    }
    const auto families = ResolveFontFamilies(fonts, css_family);
    FamilyChain chain{ .primary = families.empty() ? std::wstring{ kDefaultFamily } : families.front() };
    if (families.size() > 1) {
        std::vector<const wchar_t*> rest;
        for (size_t i = 1; i < families.size(); ++i) {
            rest.push_back(families[i].c_str());
        }
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
ComPtr<IDWriteTextFormat> CreateDiagramFormat(IDWriteFactory* dw, IDWriteFontCollection* fonts, const DiagramFont& font, const wchar_t* locale)
{
    const FamilyChain chain = ChainFor(dw, fonts, font.family);
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

void DrawRun(ID2D1DeviceContext* dc, IDWriteFactory* dw, DiagramTextMeasurer& formats, ID2D1SolidColorBrush* brush, const D2D1::Matrix3x2F& base,
             const mermaid_util::DrawingText& t)
{
    IDWriteTextFormat* format = formats.Format({ .family = t.family, .size = t.font_size, .weight = t.weight, .italic = t.italic });
    ComPtr<IDWriteTextLayout> layout;
    if (!format || t.text.empty() ||
        FAILED(dw->CreateTextLayout(t.text.data(), static_cast<UINT32>(t.text.size()), format, 1.0e6f, 1.0e6f, &layout))) {
        return;
    }
    if (t.underline) {
        layout->SetUnderline(TRUE, { 0, static_cast<UINT32>(t.text.size()) });
    }
    // ページは 1 行ずつ送るので、ベースラインは最初の行のものを使う。
    DWRITE_LINE_METRICS line{};
    UINT32 lines = 0;
    layout->GetLineMetrics(&line, 1, &lines);
    if (lines == 0) {
        return;
    }
    const auto& m = t.transform;
    dc->SetTransform(D2D1::Matrix3x2F(m[0], m[1], m[2], m[3], m[4], m[5]) * base);
    brush->SetColor(D2D1::ColorF(t.rgb, t.alpha));
    dc->DrawTextLayout(D2D1::Point2F(t.x, t.y - line.baseline), layout.Get(), brush, D2D1_DRAW_TEXT_OPTIONS_ENABLE_COLOR_FONT);
}

void AppendFontKey(std::wstring& key, const DiagramFont& font)
{
    uint32_t bits = 0;
    std::memcpy(&bits, &font.size, sizeof(bits));
    key.append(font.family);
    key.push_back(L'\0');
    key.push_back(static_cast<wchar_t>(bits & 0xFFFFu));
    key.push_back(static_cast<wchar_t>(bits >> 16));
    key.push_back(static_cast<wchar_t>(font.weight));
    key.push_back(font.italic ? L'i' : L'n');
}

// ---------------------------------------------------------------------------------------------------------------
// pele の SVG の解決。ブラウザ (mermaid.html の drawingOf) が getComputedStyle などで求めていた値を自分で求める。
// pele の出力は語彙が狭い (g・図形・text・tspan・入れ子の svg・linearGradient) ので、その範囲の CSS だけを解く。
// 利用者の style (classDef など) は pele を素通りするので、色名・hsl・単位・font-size のキーワードは読む。
// 知らない要素があれば描かずに nullopt を返し、呼び出し側がブラウザに回す。

constexpr std::string_view kSpaces = " \t\r\n"sv;

// mermaid.html の DARK_VARS と同じ。ダークの配色を SVG のルートの style の先頭に足す。
constexpr std::string_view kDarkVars =
    "--pele-bg:#1e1e1e;--pele-fg:#e0e0e0;--pele-muted:#a0a0a0;--pele-line:#d0d0d0;"
    "--pele-surface:#2b2b2b;--pele-surface-alt:#363636;--pele-border:#7a7a7a;--pele-accent:#7c9be6;"
    "--pele-series-1:#799abe;--pele-series-2:#f8a452;--pele-series-3:#7fb978;--pele-series-4:#eb8180;"
    "--pele-series-5:#95c9c5;--pele-series-6:#f2d76c;--pele-series-7:#c59bb9;--pele-series-8:#b69886;"sv;
constexpr uint32_t kLightBackground = 0xFFFFFF; // mermaid.html の body の背景
constexpr uint32_t kDarkBackground = 0x1E1E1E;

std::string_view Trim(std::string_view s) noexcept
{
    const auto first = s.find_first_not_of(kSpaces);
    if (first == std::string_view::npos) {
        return {};
    }
    return s.substr(first, s.find_last_not_of(kSpaces) - first + 1);
}

// 表の名前のように、どちらも実行時に決まる文字列の比較。片方が小文字のリテラルなら ascii_util::iequal。
bool IEquals(std::string_view a, std::string_view b) noexcept
{
    return a.size() == b.size() && std::ranges::equal(a, b, {}, ascii_util::ToLowerAscii, ascii_util::ToLowerAscii);
}

bool IsSpace(char c) noexcept
{
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
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

// 空白とカンマで区切った数の並び。max を超えたか読めなければ false。
bool ParseNumberList(std::string_view s, std::vector<double>& out, size_t max)
{
    out.clear();
    for (;;) {
        SkipSeparators(s);
        if (s.empty()) {
            return true;
        }
        double v = 0.0;
        if (out.size() >= max || !TakeNumber(s, v)) {
            return false;
        }
        out.push_back(v);
    }
}

// separator で分ける。括弧と引用符の中では分けない。
std::vector<std::string_view> SplitTopLevel(std::string_view s, char separator)
{
    std::vector<std::string_view> parts;
    int depth = 0;
    char quote = 0;
    size_t start = 0;
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
            --depth;
        }
        else if (c == separator && depth == 0) {
            parts.push_back(s.substr(start, i - start));
            start = i + 1;
        }
    }
    parts.push_back(s.substr(start));
    return parts;
}

// 長さ。単位なしと px は CSS px。% は相手が要るので読めない扱い。pele は小さな箱の角丸に min() を使う。
bool ParseLength(std::string_view v, float font_size, float& out)
{
    v = Trim(v);
    const bool is_min = ascii_util::istarts_with(v, "min(");
    if ((is_min || ascii_util::istarts_with(v, "max(")) && v.ends_with(')')) {
        bool any = false;
        for (const auto arg : SplitTopLevel(v.substr(4, v.size() - 5), ',')) {
            float x = 0.0f;
            if (!ParseLength(arg, font_size, x)) {
                return false;
            }
            out = !any ? x : is_min ? std::min(out, x) : std::max(out, x);
            any = true;
        }
        return any;
    }
    double n = 0.0;
    if (!TakeNumber(v, n)) {
        return false;
    }
    double scale = 1.0;
    if (v.empty() || ascii_util::iequal(v, "px")) {
    }
    else if (ascii_util::iequal(v, "em")) {
        scale = font_size;
    }
    else if (ascii_util::iequal(v, "ex")) {
        scale = font_size * 0.5;
    }
    else if (ascii_util::iequal(v, "pt")) {
        scale = 96.0 / 72.0;
    }
    else if (ascii_util::iequal(v, "pc")) {
        scale = 16.0;
    }
    else if (ascii_util::iequal(v, "in")) {
        scale = 96.0;
    }
    else if (ascii_util::iequal(v, "cm")) {
        scale = 96.0 / 2.54;
    }
    else if (ascii_util::iequal(v, "mm")) {
        scale = 96.0 / 25.4;
    }
    else {
        return false;
    }
    out = static_cast<float>(n * scale);
    return std::isfinite(out);
}

// SVG の matrix(a b c d e f)。点 (x, y) を (a x + c y + e, b x + d y + f) に移す。DOMMatrix と同じく double で合成する。
struct Mat {
    double a = 1.0, b = 0.0, c = 0.0, d = 1.0, e = 0.0, f = 0.0;

    std::pair<double, double> Apply(double x, double y) const noexcept
    {
        return { a * x + c * y + e, b * x + d * y + f };
    }
};

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
    std::vector<double> v;
    for (;;) {
        SkipSeparators(s);
        if (s.empty()) {
            return true;
        }
        const auto open = s.find('(');
        const auto close = s.find(')');
        if (open == std::string_view::npos || close == std::string_view::npos || close < open ||
            !ParseNumberList(s.substr(open + 1, close - open - 1), v, 6)) {
            return false;
        }
        const auto name = Trim(s.substr(0, open));
        s.remove_prefix(close + 1);
        Mat m;
        if (name == "matrix" && v.size() == 6) {
            m = { v[0], v[1], v[2], v[3], v[4], v[5] };
        }
        else if (name == "translate" && (v.size() == 1 || v.size() == 2)) {
            m.e = v[0];
            m.f = v.size() == 2 ? v[1] : 0.0;
        }
        else if (name == "scale" && (v.size() == 1 || v.size() == 2)) {
            m.a = v[0];
            m.d = v.size() == 2 ? v[1] : v[0];
        }
        else if (name == "rotate" && (v.size() == 1 || v.size() == 3)) {
            const double cs = std::cos(v[0] * kDeg);
            const double sn = std::sin(v[0] * kDeg);
            m = { cs, sn, -sn, cs, 0.0, 0.0 };
            if (v.size() == 3) {
                m = Compose(Compose(Mat{ .e = v[1], .f = v[2] }, m), Mat{ .e = -v[1], .f = -v[2] });
            }
        }
        else if (name == "skewX" && v.size() == 1) {
            m.c = std::tan(v[0] * kDeg);
        }
        else if (name == "skewY" && v.size() == 1) {
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

// path の d の外接矩形 (曲線は制御点を含める)。読めないところで止め、そこまでの範囲を返す (ブラウザも誤りの手前まで描く)。
Bounds PathBounds(std::string_view d)
{
    Bounds out;
    double x = 0.0, y = 0.0, start_x = 0.0, start_y = 0.0;
    char command = 0;
    const auto number = [&](double& v) {
        SkipSeparators(d);
        return TakeNumber(d, v);
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
            return out;
        }
        if ((d[0] >= 'A' && d[0] <= 'Z') || (d[0] >= 'a' && d[0] <= 'z')) {
            command = d[0];
            d.remove_prefix(1);
        }
        else if (command == 0) {
            return out;
        }
        const bool rel = command >= 'a';
        const double ox = rel ? x : 0.0;
        const double oy = rel ? y : 0.0;
        double v[6]{};
        switch (command & ~0x20) {
        case 'Z':
            x = start_x;
            y = start_y;
            command = 0;
            continue;
        case 'M':
        case 'L':
        case 'T':
            if (!number(v[0]) || !number(v[1])) {
                return out;
            }
            x = ox + v[0];
            y = oy + v[1];
            if ((command & ~0x20) == 'M') {
                start_x = x;
                start_y = y;
                command = rel ? 'l' : 'L';
            }
            out.Add(x, y);
            break;
        case 'H':
            if (!number(v[0])) {
                return out;
            }
            x = ox + v[0];
            out.Add(x, y);
            break;
        case 'V':
            if (!number(v[0])) {
                return out;
            }
            y = oy + v[0];
            out.Add(x, y);
            break;
        case 'C':
        case 'S':
        case 'Q': {
            const int pairs = (command & ~0x20) == 'C' ? 3 : 2;
            for (int i = 0; i < pairs; ++i) {
                if (!number(v[i * 2]) || !number(v[i * 2 + 1])) {
                    return out;
                }
                out.Add(ox + v[i * 2], oy + v[i * 2 + 1]);
            }
            x = ox + v[pairs * 2 - 2];
            y = oy + v[pairs * 2 - 1];
            break;
        }
        case 'A': {
            bool large = false;
            bool sweep = false;
            if (!number(v[0]) || !number(v[1]) || !number(v[2]) || !flag(large) || !flag(sweep) || !number(v[3]) || !number(v[4])) {
                return out;
            }
            AddArcBounds(out, x, y, v[0], v[1], v[2], large, sweep, ox + v[3], oy + v[4]);
            x = ox + v[3];
            y = oy + v[4];
            break;
        }
        default:
            return out;
        }
    }
}

// ---- XML ----

// 値と文字は、実体参照が無ければ元の SVG を指し、あれば XmlDoc::decoded を指す。
struct XmlAttr {
    std::string_view name;
    std::string_view value;
};

// name が空の節は文字。
struct XmlNode {
    std::string_view name;
    std::string_view text;
    std::vector<XmlAttr> attrs;
    std::vector<uint32_t> children;

    std::optional<std::string_view> Attr(std::string_view n) const noexcept
    {
        const auto it = std::ranges::find(attrs, n, &XmlAttr::name);
        return it == attrs.end() ? std::nullopt : std::optional{ it->value };
    }
};

// 根の要素は nodes[0]。名前は元の SVG を指すので、SVG は XmlDoc より長く生かす。
struct XmlDoc {
    std::vector<XmlNode> nodes;
    std::deque<std::string> decoded;
};

std::optional<std::string_view> DecodeXmlText(std::string_view in, XmlDoc& doc)
{
    if (in.find('&') == std::string_view::npos) {
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
    const auto add = [&](XmlNode&& node) {
        const auto index = static_cast<uint32_t>(nodes.size());
        if (!open.empty()) {
            nodes[open.back()].children.push_back(index);
        }
        nodes.push_back(std::move(node));
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
            element.attrs.push_back({ name, *value });
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
    return open.empty() && !nodes.empty();
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
    { "aliceblue", 0xf0f8ff }, { "antiquewhite", 0xfaebd7 }, { "aqua", 0x00ffff }, { "aquamarine", 0x7fffd4 }, { "azure", 0xf0ffff },
    { "beige", 0xf5f5dc }, { "bisque", 0xffe4c4 }, { "black", 0x000000 }, { "blanchedalmond", 0xffebcd }, { "blue", 0x0000ff },
    { "blueviolet", 0x8a2be2 }, { "brown", 0xa52a2a }, { "burlywood", 0xdeb887 }, { "cadetblue", 0x5f9ea0 }, { "chartreuse", 0x7fff00 },
    { "chocolate", 0xd2691e }, { "coral", 0xff7f50 }, { "cornflowerblue", 0x6495ed }, { "cornsilk", 0xfff8dc }, { "crimson", 0xdc143c },
    { "cyan", 0x00ffff }, { "darkblue", 0x00008b }, { "darkcyan", 0x008b8b }, { "darkgoldenrod", 0xb8860b }, { "darkgray", 0xa9a9a9 },
    { "darkgreen", 0x006400 }, { "darkgrey", 0xa9a9a9 }, { "darkkhaki", 0xbdb76b }, { "darkmagenta", 0x8b008b },
    { "darkolivegreen", 0x556b2f }, { "darkorange", 0xff8c00 }, { "darkorchid", 0x9932cc }, { "darkred", 0x8b0000 },
    { "darksalmon", 0xe9967a }, { "darkseagreen", 0x8fbc8f }, { "darkslateblue", 0x483d8b }, { "darkslategray", 0x2f4f4f },
    { "darkslategrey", 0x2f4f4f }, { "darkturquoise", 0x00ced1 }, { "darkviolet", 0x9400d3 }, { "deeppink", 0xff1493 },
    { "deepskyblue", 0x00bfff }, { "dimgray", 0x696969 }, { "dimgrey", 0x696969 }, { "dodgerblue", 0x1e90ff }, { "firebrick", 0xb22222 },
    { "floralwhite", 0xfffaf0 }, { "forestgreen", 0x228b22 }, { "fuchsia", 0xff00ff }, { "gainsboro", 0xdcdcdc }, { "ghostwhite", 0xf8f8ff },
    { "gold", 0xffd700 }, { "goldenrod", 0xdaa520 }, { "gray", 0x808080 }, { "green", 0x008000 }, { "greenyellow", 0xadff2f },
    { "grey", 0x808080 }, { "honeydew", 0xf0fff0 }, { "hotpink", 0xff69b4 }, { "indianred", 0xcd5c5c }, { "indigo", 0x4b0082 },
    { "ivory", 0xfffff0 }, { "khaki", 0xf0e68c }, { "lavender", 0xe6e6fa }, { "lavenderblush", 0xfff0f5 }, { "lawngreen", 0x7cfc00 },
    { "lemonchiffon", 0xfffacd }, { "lightblue", 0xadd8e6 }, { "lightcoral", 0xf08080 }, { "lightcyan", 0xe0ffff },
    { "lightgoldenrodyellow", 0xfafad2 }, { "lightgray", 0xd3d3d3 }, { "lightgreen", 0x90ee90 }, { "lightgrey", 0xd3d3d3 },
    { "lightpink", 0xffb6c1 }, { "lightsalmon", 0xffa07a }, { "lightseagreen", 0x20b2aa }, { "lightskyblue", 0x87cefa },
    { "lightslategray", 0x778899 }, { "lightslategrey", 0x778899 }, { "lightsteelblue", 0xb0c4de }, { "lightyellow", 0xffffe0 },
    { "lime", 0x00ff00 }, { "limegreen", 0x32cd32 }, { "linen", 0xfaf0e6 }, { "magenta", 0xff00ff }, { "maroon", 0x800000 },
    { "mediumaquamarine", 0x66cdaa }, { "mediumblue", 0x0000cd }, { "mediumorchid", 0xba55d3 }, { "mediumpurple", 0x9370db },
    { "mediumseagreen", 0x3cb371 }, { "mediumslateblue", 0x7b68ee }, { "mediumspringgreen", 0x00fa9a }, { "mediumturquoise", 0x48d1cc },
    { "mediumvioletred", 0xc71585 }, { "midnightblue", 0x191970 }, { "mintcream", 0xf5fffa }, { "mistyrose", 0xffe4e1 },
    { "moccasin", 0xffe4b5 }, { "navajowhite", 0xffdead }, { "navy", 0x000080 }, { "oldlace", 0xfdf5e6 }, { "olive", 0x808000 },
    { "olivedrab", 0x6b8e23 }, { "orange", 0xffa500 }, { "orangered", 0xff4500 }, { "orchid", 0xda70d6 }, { "palegoldenrod", 0xeee8aa },
    { "palegreen", 0x98fb98 }, { "paleturquoise", 0xafeeee }, { "palevioletred", 0xdb7093 }, { "papayawhip", 0xffefd5 },
    { "peachpuff", 0xffdab9 }, { "peru", 0xcd853f }, { "pink", 0xffc0cb }, { "plum", 0xdda0dd }, { "powderblue", 0xb0e0e6 },
    { "purple", 0x800080 }, { "rebeccapurple", 0x663399 }, { "red", 0xff0000 }, { "rosybrown", 0xbc8f8f }, { "royalblue", 0x4169e1 },
    { "saddlebrown", 0x8b4513 }, { "salmon", 0xfa8072 }, { "sandybrown", 0xf4a460 }, { "seagreen", 0x2e8b57 }, { "seashell", 0xfff5ee },
    { "sienna", 0xa0522d }, { "silver", 0xc0c0c0 }, { "skyblue", 0x87ceeb }, { "slateblue", 0x6a5acd }, { "slategray", 0x708090 },
    { "slategrey", 0x708090 }, { "snow", 0xfffafa }, { "springgreen", 0x00ff7f }, { "steelblue", 0x4682b4 }, { "tan", 0xd2b48c },
    { "teal", 0x008080 }, { "thistle", 0xd8bfd8 }, { "tomato", 0xff6347 }, { "turquoise", 0x40e0d0 }, { "violet", 0xee82ee },
    { "wheat", 0xf5deb3 }, { "white", 0xffffff }, { "whitesmoke", 0xf5f5f5 }, { "yellow", 0xffff00 }, { "yellowgreen", 0x9acd32 },
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
    out = static_cast<float>(v);
    return true;
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
    const auto parts = SplitTopLevel(args, ',');
    if (parts.size() != 3 || !ascii_util::iequal(Trim(parts[0]), "in srgb")) {
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
        if (v.size() > 32) {
            return false;
        }
        const std::string lower = ascii_util::ToLowerAsciiCopy(v);
        const auto it = std::ranges::lower_bound(kNamedColors, std::string_view{ lower }, {}, &NamedColor::name);
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

uint32_t ToRgb(const Rgba& c) noexcept
{
    const auto byte = [](float v) { return static_cast<uint32_t>(std::lround(std::clamp(v, 0.0f, 1.0f) * 255.0f)); };
    return (byte(c.r) << 16) | (byte(c.g) << 8) | byte(c.b);
}

void AppendHexColor(std::string& out, const Rgba& c)
{
    constexpr char kHex[] = "0123456789abcdef";
    const uint32_t rgb = ToRgb(c);
    out += '#';
    for (int shift = 20; shift >= 0; shift -= 4) {
        out += kHex[(rgb >> shift) & 0xF];
    }
}

// mermaid.html の num() と同じく、小数 3 桁に丸めて短く書く。
void AppendNum(std::string& out, double v)
{
    double r = std::round(v * 1000.0) / 1000.0;
    if (r == 0.0) {
        r = 0.0;
    }
    char buf[32];
    const auto [ptr, ec] = std::to_chars(buf, buf + sizeof(buf), r);
    out.append(buf, ec == std::errc{} ? ptr : buf);
}

// 二重引用符で囲む属性値に書く。
void AppendEscaped(std::string& out, std::string_view s)
{
    for (const char c : s) {
        switch (c) {
        case '&':
            out += "&amp;";
            break;
        case '<':
            out += "&lt;";
            break;
        case '"':
            out += "&quot;";
            break;
        default:
            out += c;
        }
    }
}

void AppendAttr(std::string& out, std::string_view name, double v)
{
    out += ' ';
    out += name;
    out += "=\"";
    AppendNum(out, v);
    out += '"';
}

void AppendAttr(std::string& out, std::string_view name, std::string_view v)
{
    out += ' ';
    out += name;
    out += "=\"";
    AppendEscaped(out, v);
    out += '"';
}

void AppendAttr(std::string& out, std::string_view name, const Rgba& c)
{
    out += ' ';
    out += name;
    out += "=\"";
    AppendHexColor(out, c);
    out += '"';
}

// ---- スタイル ----

struct Paint {
    enum class Kind : uint8_t { None, Color, CurrentColor, Url };
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
        auto ref = Trim(v.substr(4, close - 4));
        if (ref.size() >= 2 && (ref.front() == '"' || ref.front() == '\'') && ref.back() == ref.front()) {
            ref = ref.substr(1, ref.size() - 2);
        }
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
    { "color", Prop::Color },
    { "font-size", Prop::FontSize },
    { "fill", Prop::Fill },
    { "stroke", Prop::Stroke },
    { "fill-opacity", Prop::FillOpacity },
    { "stroke-opacity", Prop::StrokeOpacity },
    { "stroke-width", Prop::StrokeWidth },
    { "stroke-dasharray", Prop::StrokeDasharray },
    { "stroke-dashoffset", Prop::StrokeDashoffset },
    { "stroke-linecap", Prop::StrokeLinecap },
    { "stroke-linejoin", Prop::StrokeLinejoin },
    { "stroke-miterlimit", Prop::StrokeMiterlimit },
    { "fill-rule", Prop::FillRule },
    { "opacity", Prop::Opacity },
    { "display", Prop::Display },
    { "visibility", Prop::Visibility },
    { "font-family", Prop::FontFamily },
    { "font-weight", Prop::FontWeight },
    { "font-style", Prop::FontStyle },
    { "text-anchor", Prop::TextAnchor },
    { "text-decoration", Prop::TextDecoration },
    { "text-decoration-line", Prop::TextDecoration },
    { "baseline-shift", Prop::BaselineShift },
    { "rx", Prop::Rx },
    { "ry", Prop::Ry },
    { "stop-color", Prop::StopColor },
    { "stop-opacity", Prop::StopOpacity },
    { "max-width", Prop::MaxWidth },
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
    for (size_t i = 0; i + 4 <= s.size(); ++i) {
        if (ascii_util::istarts_with(s.substr(i), "var(")) {
            return i;
        }
    }
    return std::string_view::npos;
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
            level += in[i] == '(' ? 1 : in[i] == ')' ? -1 : 0;
            if (level == 0) {
                close = i;
                break;
            }
        }
        if (close == std::string_view::npos) {
            return false;
        }
        const auto inner = in.substr(pos + 4, close - pos - 4);
        const auto parts = SplitTopLevel(inner, ',');
        const auto name = Trim(parts[0]);
        bool done = false;
        if (const auto* value = vars ? vars->Find(name) : nullptr) {
            std::string resolved;
            if (SubstituteVars(*value, vars, resolved, depth + 1)) {
                out += resolved;
                done = true;
            }
        }
        // 参照先が無効なら代わりの値を使う。
        if (!done && (parts.size() < 2 || !SubstituteVars(Trim(inner.substr(parts[0].size() + 1)), vars, out, depth + 1))) {
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
    // 継承しない
    float opacity = 1.0f;
    bool display_none = false;
    bool underline = false;
    bool fit_width = false; // max-width:100%
    std::string_view baseline_shift;
    std::optional<float> rx; // nullopt は auto
    std::optional<float> ry;
    Rgba stop_color{};
    float stop_opacity = 1.0f;

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
        return parent < 350 ? 400 : parent < 550 ? 700 : 900;
    }
    if (ascii_util::iequal(v, "lighter")) {
        return parent < 550 ? 100 : parent < 750 ? 400 : 700;
    }
    return 0;
}

// font-size。smaller・larger は Chromium と同じく 1.2 倍 (pele の measurer の下付き・上付きも 1.2 で割る)。
bool ParseFontSize(std::string_view v, float parent, float& out)
{
    constexpr std::pair<std::string_view, float> kKeywords[]{
        { "xx-small", 9.0f }, { "x-small", 10.0f }, { "small", 13.0f }, { "medium", 16.0f },
        { "large", 18.0f },   { "x-large", 24.0f }, { "xx-large", 32.0f },
    };
    v = Trim(v);
    if (ascii_util::iequal(v, "smaller")) {
        out = parent / 1.2f;
        return true;
    }
    if (ascii_util::iequal(v, "larger")) {
        out = parent * 1.2f;
        return true;
    }
    for (const auto& [name, size] : kKeywords) {
        if (IEquals(v, name)) {
            out = size;
            return true;
        }
    }
    if (v.ends_with('%')) {
        return ParseNumberOrPercent(v, parent, out) && out >= 0.0f;
    }
    return ParseLength(v, parent, out) && out >= 0.0f;
}

// 属性の長さ。無いか読めなければ nullopt (ブラウザは読めない値を初期値の 0 にする)。
std::optional<float> AttrLength(const XmlNode& el, std::string_view name, float font_size)
{
    float v = 0.0f;
    const auto attr = el.Attr(name);
    return attr && ParseLength(*attr, font_size, v) ? std::optional{ v } : std::nullopt;
}

// viewBox と preserveAspectRatio による、(x, y, width, height) の枠への変換。根と入れ子の svg に使う。
bool ViewportTransform(const XmlNode& el, double x, double y, double width, double height, Mat& out)
{
    out = { .e = x, .f = y };
    const auto view_box = el.Attr("viewBox");
    if (!view_box) {
        return true;
    }
    std::vector<double> vb;
    if (!ParseNumberList(*view_box, vb, 4) || vb.size() != 4 || !(vb[2] > 0.0 && vb[3] > 0.0)) {
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
        const double fx = align.find("xMid") != std::string_view::npos ? 0.5 : align.find("xMax") != std::string_view::npos ? 1.0 : 0.0;
        const double fy = align.find("YMid") != std::string_view::npos ? 0.5 : align.find("YMax") != std::string_view::npos ? 1.0 : 0.0;
        tx = (width - vb[2] * sx) * fx;
        ty = (height - vb[3] * sy) * fy;
    }
    out = { sx, 0.0, 0.0, sy, x + tx - vb[0] * sx, y + ty - vb[1] * sy };
    return true;
}

class PeleSvgResolver {
public:
    PeleSvgResolver(const XmlDoc& doc, DiagramTextMeasurer& measurer) : nodes_(doc.nodes), measurer_(measurer)
    {
    }

    std::optional<mermaid_util::DiagramDrawing> Run(bool dark, float max_width);

private:
    struct Layer {
        std::string shapes;
        std::vector<mermaid_util::DrawingText> texts;
        std::vector<Bounds> text_bounds;
    };
    struct Piece {
        std::string_view raw;
        Style style;
        float opacity = 1.0f;
        bool underline = false;
        float shift = 0.0f; // baseline-shift の合計 (下が正)
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
    Style Compute(const Style& parent, const XmlNode& el, std::string_view extra_style = {});
    void Apply(Style& s, const Style& parent, Prop prop, std::string_view value);
    bool Walk(uint32_t index, const Style& parent, const Mat& ctm, float opacity);
    bool Shape(const XmlNode& el, const Style& s, const Mat& ctm, float opacity);
    bool Text(uint32_t index, const Style& s, const Mat& ctm, float opacity);
    bool CollectText(uint32_t index, const Style& s, float opacity, bool underline, float shift, std::vector<Piece>& pieces,
                     std::optional<float>& pending_x, std::optional<float>& pending_y);
    void AppendPaint(std::string& out, std::string_view name, const Paint& paint, const Style& s, float paint_opacity);
    std::string_view GradientRef(std::string_view id, float alpha);
    float BaselineShift(const Style& s);
    std::string& ShapesFor(const Bounds& bounds);

    const std::vector<XmlNode>& nodes_;
    DiagramTextMeasurer& measurer_;
    std::deque<std::string> strings_;
    std::deque<std::wstring> wide_strings_;
    std::deque<Vars> vars_;
    std::unordered_map<std::string_view, uint32_t> ids_;
    std::vector<std::pair<std::string, std::string>> gradients_; // (id|alpha, 名前。空は塗れない参照)
    std::string defs_;
    std::vector<Layer> layers_{ 1 };
    Style root_style_;
};

void PeleSvgResolver::Apply(Style& s, const Style& parent, Prop prop, std::string_view value)
{
    value = Trim(value);
    if (const auto important = value.find('!'); important != std::string_view::npos) {
        value = Trim(value.substr(0, important));
    }
    if (ascii_util::iequal(value, "inherit")) {
        // 継承するものは既に親の値。継承しないものは親から写す。
        switch (prop) {
        case Prop::Opacity:
            s.opacity = parent.opacity;
            break;
        case Prop::BaselineShift:
            s.baseline_shift = parent.baseline_shift;
            break;
        case Prop::Rx:
            s.rx = parent.rx;
            break;
        case Prop::Ry:
            s.ry = parent.ry;
            break;
        case Prop::StopColor:
            s.stop_color = parent.stop_color;
            break;
        case Prop::StopOpacity:
            s.stop_opacity = parent.stop_opacity;
            break;
        default:
            break;
        }
        return;
    }
    float f = 0.0f;
    switch (prop) {
    case Prop::Color:
        ParseColor(value, parent.color, s.color);
        break;
    case Prop::FontSize:
        if (ParseFontSize(value, parent.font_size, f)) {
            s.font_size = f;
        }
        break;
    case Prop::Fill:
        ParsePaint(value, s.color, s.fill);
        break;
    case Prop::Stroke:
        ParsePaint(value, s.color, s.stroke);
        break;
    case Prop::FillOpacity:
        ParseOpacityValue(value, s.fill_opacity);
        break;
    case Prop::StrokeOpacity:
        ParseOpacityValue(value, s.stroke_opacity);
        break;
    case Prop::Opacity:
        ParseOpacityValue(value, s.opacity);
        break;
    case Prop::StopOpacity:
        ParseOpacityValue(value, s.stop_opacity);
        break;
    case Prop::StopColor:
        ParseColor(value, s.color, s.stop_color);
        break;
    case Prop::StrokeWidth:
        if (ParseLength(value, s.font_size, f) && f >= 0.0f) {
            s.stroke_width = f;
        }
        break;
    case Prop::StrokeDashoffset:
        ParseLength(value, s.font_size, s.dash_offset);
        break;
    case Prop::StrokeMiterlimit:
        if (ParseLength(value, s.font_size, f) && f >= 1.0f) {
            s.miter_limit = f;
        }
        break;
    case Prop::StrokeDasharray: {
        if (ascii_util::iequal(value, "none")) {
            s.dash_array = {};
            break;
        }
        std::string normalized;
        bool any = false;
        for (const auto part : FunctionTokens(value)) {
            if (!ParseLength(part, s.font_size, f) || f < 0.0f) {
                return;
            }
            any = any || f > 0.0f;
            if (!normalized.empty()) {
                normalized += ' ';
            }
            AppendNum(normalized, f);
        }
        s.dash_array = any ? Keep(std::move(normalized)) : std::string_view{};
        break;
    }
    case Prop::StrokeLinecap:
        for (const std::string_view v : { "butt"sv, "round"sv, "square"sv }) {
            if (IEquals(value, v)) {
                s.line_cap = v;
            }
        }
        break;
    case Prop::StrokeLinejoin:
        for (const std::string_view v : { "miter"sv, "round"sv, "bevel"sv }) {
            if (IEquals(value, v)) {
                s.line_join = v;
            }
        }
        break;
    case Prop::FillRule:
        s.even_odd = ascii_util::iequal(value, "evenodd");
        break;
    case Prop::Display:
        s.display_none = ascii_util::iequal(value, "none");
        break;
    case Prop::Visibility:
        s.visible = ascii_util::iequal(value, "visible");
        break;
    case Prop::FontFamily:
        if (!value.empty()) {
            s.font_family = wide_strings_.emplace_back(string_convert::Utf8ToWide(value));
        }
        break;
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
        break;
    }
    case Prop::FontStyle:
        s.italic = ascii_util::iequal(value, "italic") || ascii_util::istarts_with(value, "oblique");
        break;
    case Prop::TextAnchor:
        s.anchor = ascii_util::iequal(value, "middle") ? 1 : ascii_util::iequal(value, "end") ? 2 : 0;
        break;
    case Prop::TextDecoration:
        s.underline = value.find("underline") != std::string_view::npos;
        break;
    case Prop::BaselineShift:
        s.baseline_shift = value;
        break;
    case Prop::Rx:
    case Prop::Ry: {
        auto& r = prop == Prop::Rx ? s.rx : s.ry;
        r = ascii_util::iequal(value, "auto") || !ParseLength(value, s.font_size, f) ? std::nullopt : std::optional{ f };
        break;
    }
    case Prop::MaxWidth:
        s.fit_width = value == "100%";
        break;
    case Prop::Count:
        break;
    }
}

Style PeleSvgResolver::Compute(const Style& parent, const XmlNode& el, std::string_view extra_style)
{
    Style s = parent;
    s.opacity = 1.0f;
    s.display_none = false;
    s.underline = false;
    s.fit_width = false;
    s.baseline_shift = {};
    s.rx = s.ry = std::nullopt;
    s.stop_color = {};
    s.stop_opacity = 1.0f;

    // 表示の属性 < style 属性。style の中では後の宣言が勝つ。
    std::array<std::optional<std::string_view>, kPropCount> specified{};
    for (const auto& attr : el.attrs) {
        if (attr.name == "xml:space") {
            s.preserve_space = attr.value == "preserve";
        }
        else if (attr.name != "style") {
            if (const auto p = PropOf(attr.name)) {
                specified[static_cast<size_t>(*p)] = attr.value;
            }
        }
    }
    const auto set = [&](Prop p, std::string_view v) { specified[static_cast<size_t>(p)] = v; };
    Vars* own_vars = nullptr;
    const auto declarations = [&](std::string_view style) {
        for (const auto decl : SplitTopLevel(style, ';')) {
            const auto colon = decl.find(':');
            if (colon == std::string_view::npos) {
                continue;
            }
            const auto name = Trim(decl.substr(0, colon));
            const auto value = Trim(decl.substr(colon + 1));
            if (name.starts_with("--")) {
                if (!own_vars) {
                    own_vars = &vars_.emplace_back(Vars{ .parent = parent.vars });
                }
                own_vars->values.emplace_back(name, value);
            }
            else if (ascii_util::iequal(name, "font")) {
                // [style] [weight] size[/line-height] family
                std::string_view rest = value;
                set(Prop::FontStyle, "normal");
                set(Prop::FontWeight, "normal");
                while (!rest.empty()) {
                    const size_t end = std::min(rest.find_first_of(kSpaces), rest.size());
                    const auto token = rest.substr(0, end);
                    const auto size_token = token.substr(0, token.find('/'));
                    float size = 0.0f;
                    if (ParseFontSize(size_token, parent.font_size, size)) {
                        set(Prop::FontSize, size_token);
                        set(Prop::FontFamily, Trim(rest.substr(end)));
                        break;
                    }
                    if (ascii_util::iequal(token, "italic") || ascii_util::iequal(token, "oblique")) {
                        set(Prop::FontStyle, token);
                    }
                    else if (!ascii_util::iequal(token, "normal")) {
                        set(Prop::FontWeight, token);
                    }
                    rest = Trim(rest.substr(end));
                }
            }
            else if (const auto p = PropOf(name)) {
                set(*p, value);
            }
        }
    };
    declarations(extra_style);
    if (const auto style = el.Attr("style")) {
        declarations(*style);
    }
    if (own_vars) {
        s.vars = own_vars;
    }
    for (size_t i = 0; i < kPropCount; ++i) {
        if (!specified[i]) {
            continue;
        }
        std::string_view value = *specified[i];
        if (FindVar(value) != std::string_view::npos) {
            std::string resolved;
            if (!SubstituteVars(value, s.vars, resolved)) {
                // 計算値の時点で無効。継承するものは親の値のまま、しないものは初期値のまま。
                continue;
            }
            value = Keep(std::move(resolved));
        }
        Apply(s, parent, static_cast<Prop>(i), value);
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
    AppendAttr(out, std::string{ name } + "-opacity", color->a * paint_opacity);
}

std::string_view PeleSvgResolver::GradientRef(std::string_view id, float alpha)
{
    std::string key{ id };
    key += '|';
    AppendNum(key, alpha);
    if (const auto it = std::ranges::find(gradients_, key, &std::pair<std::string, std::string>::first); it != gradients_.end()) {
        return it->second;
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
        AppendAttr(defs_, "stop-color", ss.stop_color);
        AppendAttr(defs_, "stop-opacity", ss.stop_color.a * ss.stop_opacity * alpha);
        defs_ += "/>";
    }
    defs_ += "</";
    defs_ += g->name;
    defs_ += '>';
    gradients_.emplace_back(std::move(key), std::move(name));
    return gradients_.back().second;
}

// 図形を書く層。図形は今の層の文字に重なるときだけ次の層へ送り、文書の順の重なりを保つ。
std::string& PeleSvgResolver::ShapesFor(const Bounds& bounds)
{
    if (std::ranges::any_of(layers_.back().text_bounds, [&](const Bounds& t) { return bounds.Overlaps(t); })) {
        layers_.emplace_back();
    }
    return layers_.back().shapes;
}

bool PeleSvgResolver::Shape(const XmlNode& el, const Style& s, const Mat& ctm, float opacity)
{
    const auto length = [&](std::string_view name) { return AttrLength(el, name, s.font_size).value_or(0.0f); };
    // rx・ry の auto はもう片方の値になる。
    const std::optional<float> rx = s.rx ? s.rx : s.ry;
    const std::optional<float> ry = s.ry ? s.ry : s.rx;
    Bounds local;
    std::array<std::pair<std::string_view, float>, 4> numbers{};
    size_t count = 0;
    std::optional<std::string_view> raw; // path の d・polygon の points
    std::vector<double> points;
    if (el.name == "path") {
        raw = el.Attr("d").value_or("");
        local = PathBounds(*raw);
    }
    else if (el.name == "rect") {
        const float x = length("x"), y = length("y"), w = length("width"), h = length("height");
        if (!(w > 0.0f && h > 0.0f)) {
            return true;
        }
        numbers = { { { "x", x }, { "y", y }, { "width", w }, { "height", h } } };
        count = 4;
        local.Add(x, y);
        local.Add(x + w, y + h);
    }
    else if (el.name == "circle") {
        const float cx = length("cx"), cy = length("cy"), r = length("r");
        if (!(r > 0.0f)) {
            return true;
        }
        numbers = { { { "cx", cx }, { "cy", cy }, { "r", r } } };
        count = 3;
        local.Add(cx - r, cy - r);
        local.Add(cx + r, cy + r);
    }
    else if (el.name == "ellipse") {
        const float cx = length("cx"), cy = length("cy");
        if (!(rx.value_or(0.0f) > 0.0f && ry.value_or(0.0f) > 0.0f)) {
            return true;
        }
        numbers = { { { "cx", cx }, { "cy", cy }, { "rx", *rx }, { "ry", *ry } } };
        count = 4;
        local.Add(cx - *rx, cy - *ry);
        local.Add(cx + *rx, cy + *ry);
    }
    else if (el.name == "line") {
        numbers = { { { "x1", length("x1") }, { "y1", length("y1") }, { "x2", length("x2") }, { "y2", length("y2") } } };
        count = 4;
        local.Add(numbers[0].second, numbers[1].second);
        local.Add(numbers[2].second, numbers[3].second);
    }
    else {
        raw = el.Attr("points").value_or("");
        if (ParseNumberList(*raw, points, std::numeric_limits<size_t>::max())) {
            for (size_t i = 0; i + 1 < points.size(); i += 2) {
                local.Add(points[i], points[i + 1]);
            }
        }
    }

    std::string& out = ShapesFor(local.Transformed(ctm).Padded(s.stroke_width));
    out += '<';
    out += el.name;
    for (size_t i = 0; i < count; ++i) {
        AppendAttr(out, numbers[i].first, numbers[i].second);
    }
    if (raw) {
        AppendAttr(out, el.name == "path" ? "d"sv : "points"sv, *raw);
    }
    if (el.name == "rect" && rx) {
        AppendAttr(out, "rx", *rx);
        AppendAttr(out, "ry", *ry);
    }
    out += " transform=\"matrix(";
    for (const double v : { ctm.a, ctm.b, ctm.c, ctm.d, ctm.e, ctm.f }) {
        AppendNum(out, v);
        out += ' ';
    }
    out.back() = ')';
    out += '"';
    AppendPaint(out, "fill", s.fill, s, s.fill_opacity);
    if (s.even_odd) {
        out += " fill-rule=\"evenodd\"";
    }
    if (s.stroke.kind != Paint::Kind::None) {
        AppendPaint(out, "stroke", s.stroke, s, s.stroke_opacity);
        AppendAttr(out, "stroke-width", s.stroke_width);
        AppendAttr(out, "stroke-linecap", s.line_cap);
        AppendAttr(out, "stroke-linejoin", s.line_join);
        AppendAttr(out, "stroke-miterlimit", s.miter_limit);
        if (!s.dash_array.empty()) {
            AppendAttr(out, "stroke-dasharray", s.dash_array);
            AppendAttr(out, "stroke-dashoffset", s.dash_offset);
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
    const auto v = s.baseline_shift;
    if (v.empty() || ascii_util::iequal(v, "baseline")) {
        return 0.0f;
    }
    if (ascii_util::iequal(v, "sub") || ascii_util::iequal(v, "super")) {
        // Chromium と同じく、その要素の書体の行の高さの半分だけずらす。
        const auto [ascent, descent] = measurer_.Extent(s.Font());
        const float half = (ascent + descent) / 2.0f;
        return ascii_util::iequal(v, "sub") ? half : -half;
    }
    float length = 0.0f;
    if (v.ends_with('%')) {
        return ParseNumberOrPercent(v, s.font_size, length) ? -length : 0.0f;
    }
    return ParseLength(v, s.font_size, length) ? -length : 0.0f;
}

bool PeleSvgResolver::CollectText(uint32_t index, const Style& s, float opacity, bool underline, float shift, std::vector<Piece>& pieces,
                                  std::optional<float>& pending_x, std::optional<float>& pending_y)
{
    const XmlNode& el = nodes_[index];
    // 文字ごとの位置 (値の並び) と回転は DirectWrite の 1 行では置けない。
    for (const std::string_view unsupported : { "dx"sv, "dy"sv, "rotate"sv, "textLength"sv }) {
        if (el.Attr(unsupported)) {
            return false;
        }
    }
    std::vector<double> numbers;
    const auto position = [&](std::string_view name, std::optional<float>& out) {
        const auto v = el.Attr(name);
        if (!v) {
            return true;
        }
        if (!ParseNumberList(*v, numbers, 1)) {
            return false;
        }
        if (!numbers.empty()) {
            out = static_cast<float>(numbers[0]);
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
                               .underline = underline,
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
        if (ts.display_none) {
            continue;
        }
        if (!CollectText(child, ts, opacity * ts.opacity, underline || ts.underline, shift + BaselineShift(ts), pieces, pending_x, pending_y)) {
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
    if (!CollectText(index, s, opacity, s.underline, 0.0f, pieces, pending_x, pending_y)) {
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
        const double dx = anchor == 1 ? -(x - chunk_left) / 2.0 : anchor == 2 ? -(x - chunk_left) : 0.0;
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

    for (Piece& piece : pieces) {
        const Style& ps = piece.style;
        const auto color = SolidColor(ps.fill, ps);
        if (piece.text.find_first_not_of(L' ') == std::wstring::npos || !ps.visible || piece.opacity <= 0.0f || !color) {
            continue;
        }
        const float baseline = piece.top + piece.shift;
        const auto [ascent, descent] = measurer_.Extent(ps.Font());
        Bounds box;
        box.Add(piece.left, baseline - ascent);
        box.Add(piece.left + piece.width, baseline + descent);
        Layer& layer = layers_.back();
        layer.text_bounds.push_back(box.Transformed(ctm));
        layer.texts.push_back({
            .transform = { static_cast<float>(ctm.a), static_cast<float>(ctm.b), static_cast<float>(ctm.c), static_cast<float>(ctm.d),
                           static_cast<float>(ctm.e), static_cast<float>(ctm.f) },
            .x = piece.left,
            .y = baseline,
            .font_size = ps.font_size,
            .weight = ps.weight,
            .italic = ps.italic,
            .underline = piece.underline,
            .rgb = ToRgb(*color),
            .alpha = std::clamp(color->a * ps.fill_opacity * piece.opacity, 0.0f, 1.0f),
            .family = std::wstring{ ps.font_family },
            .text = std::move(piece.text),
        });
    }
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
        // foreignObject (数式のラベル)・image・use・style など。ブラウザに描かせる。
        return false;
    }
    const Style s = Compute(parent, el);
    const float own_opacity = opacity * s.opacity;
    if (s.display_none || own_opacity <= 0.0f) {
        return true;
    }
    Mat m = ctm;
    if (const auto transform = el.Attr("transform")) {
        Mat t;
        if (ParseTransform(*transform, t)) {
            m = Compose(m, t);
        }
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
        for (const uint32_t child : el.children) {
            if (!Walk(child, s, m, own_opacity)) {
                return false;
            }
        }
        return true;
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
    for (uint32_t i = 0; i < nodes_.size(); ++i) {
        if (const auto id = nodes_[i].Attr("id")) {
            ids_.emplace(*id, i);
        }
    }
    root_style_ = Compute(Style{}, root, dark ? kDarkVars : ""sv);
    const Style& rs = root_style_;
    const auto attr_w = AttrLength(root, "width", rs.font_size);
    const auto attr_h = AttrLength(root, "height", rs.font_size);
    if (!(attr_w.value_or(0.0f) > 0.0f && attr_h.value_or(0.0f) > 0.0f)) {
        return std::nullopt;
    }
    float w = *attr_w;
    float h = *attr_h;
    // ページは SVG の style の max-width:100%;height:auto で、幅の上限まで縦横比を保って縮める。
    if (rs.fit_width && max_width > 0.0f && w > max_width) {
        h = h * max_width / w;
        w = max_width;
    }
    // ブラウザのレイアウトは 1/64 px に切り捨てる。大きさの切り上げをページとそろえる (837x520 を幅 800 にすると高さ 497.013 → 497)。
    w = std::floor(w * 64.0f) / 64.0f;
    h = std::floor(h * 64.0f) / 64.0f;
    Mat root_ctm;
    if (!ViewportTransform(root, 0.0, 0.0, w, h, root_ctm)) {
        return std::nullopt;
    }
    if (!rs.display_none) {
        for (const uint32_t child : root.children) {
            if (!Walk(child, rs, root_ctm, rs.opacity)) {
                return std::nullopt;
            }
        }
    }
    const float width = std::ceil(w);
    const float height = std::ceil(h);
    mermaid_util::DiagramDrawing out{
        .size = { width, height, 0.0f },
        .background = dark ? kDarkBackground : kLightBackground,
    };
    std::string head = "<svg xmlns=\"http://www.w3.org/2000/svg\"";
    AppendAttr(head, "width", width);
    AppendAttr(head, "height", height);
    head += " viewBox=\"0 0 ";
    AppendNum(head, width);
    head += ' ';
    AppendNum(head, height);
    head += "\">";
    if (!defs_.empty()) {
        head += "<defs>" + defs_ + "</defs>";
    }
    out.layers.reserve(layers_.size());
    for (Layer& layer : layers_) {
        std::string svg;
        if (!layer.shapes.empty()) {
            svg.reserve(head.size() + layer.shapes.size() + 6);
            svg += head;
            svg += layer.shapes;
            svg += "</svg>";
        }
        out.layers.push_back({ .shapes_svg = std::move(svg), .texts = std::move(layer.texts) });
    }
    return out;
}

} // namespace

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
        const std::wstring_view name = TrimFamily(css_family.substr(0, comma));
        css_family = comma == std::wstring_view::npos ? std::wstring_view{} : css_family.substr(comma + 1);
        if (name.empty()) {
            continue;
        }
        if (name == L"monospace"sv) {
            const auto mono = std::ranges::find_if(theme_palette::kMonospaceFontCandidates, [&](const wchar_t* f) { return HasFamily(fonts, f); });
            if (mono != std::end(theme_palette::kMonospaceFontCandidates)) {
                add(*mono);
            }
            continue;
        }
        const auto generic = std::ranges::find(kGenericFamilies, name, &std::pair<std::wstring_view, std::wstring_view>::first);
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
{
    if (dw_) {
        dw_->GetSystemFontCollection(&fonts_);
    }
}

DiagramTextMeasurer::~DiagramTextMeasurer() = default;

DiagramTextMeasurer::FontEntry& DiagramTextMeasurer::Entry(const DiagramFont& font)
{
    key_.clear();
    AppendFontKey(key_, font);
    auto it = entries_.find(key_);
    if (it == entries_.end()) {
        it = entries_.emplace(key_, FontEntry{ .format = dw_ ? CreateDiagramFormat(dw_.Get(), fonts_.Get(), font, locale_.c_str()) : nullptr }).first;
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

std::optional<mermaid_util::DiagramDrawing> DrawingFromPeleSvg(std::string_view svg, bool dark, float max_width, DiagramTextMeasurer& measurer)
{
    XmlDoc doc;
    if (!ParseXml(svg, doc)) {
        return std::nullopt;
    }
    return PeleSvgResolver(doc, measurer).Run(dark, max_width);
}

Microsoft::WRL::ComPtr<IWICBitmap> RasterizeDiagram(ID2D1Factory* d2d, IDWriteFactory* dw, IWICImagingFactory* wic,
                                                    const mermaid_util::DiagramDrawing& drawing, float dpr, const wchar_t* locale)
{
    const auto width = static_cast<UINT>(std::ceil(drawing.size.width * dpr));
    const auto height = static_cast<UINT>(std::ceil(drawing.size.height * dpr));
    if (!d2d || !dw || !wic || width == 0 || height == 0) {
        return nullptr;
    }
    ComPtr<IWICBitmap> bitmap;
    if (FAILED(wic->CreateBitmap(width, height, GUID_WICPixelFormat32bppPBGRA, WICBitmapCacheOnLoad, &bitmap))) {
        return nullptr;
    }
    const float dpi = 96.0f * dpr;
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

bool DrawDiagram(ID2D1DeviceContext5* dc, IDWriteFactory* dw, IWICImagingFactory* wic, const mermaid_util::DiagramDrawing& drawing,
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
    dc->FillRectangle(D2D1::RectF(0.0f, 0.0f, drawing.size.width, drawing.size.height), brush.Get());
    DiagramTextMeasurer formats(dw, locale ? locale : L"");
    bool ok = true;
    for (const auto& layer : drawing.layers) {
        if (!layer.shapes_svg.empty()) {
            const std::span bytes{ reinterpret_cast<const uint8_t*>(layer.shapes_svg.data()), layer.shapes_svg.size() };
            const auto stream = wic_util::CreateStreamOnMemory(wic, bytes);
            ComPtr<ID2D1SvgDocument> doc;
            if (!stream || FAILED(dc->CreateSvgDocument(stream.Get(), D2D1::SizeF(drawing.size.width, drawing.size.height), &doc))) {
                ok = false;
                break;
            }
            dc->SetTransform(base);
            dc->DrawSvgDocument(doc.Get());
        }
        for (const auto& t : layer.texts) {
            DrawRun(dc, dw, formats, brush.Get(), base, t);
        }
    }
    dc->SetTransform(base);
    return ok;
}
