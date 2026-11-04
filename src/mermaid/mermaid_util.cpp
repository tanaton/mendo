#include "mermaid_util.h"
#include "document_types.h"
#include "utility.h"
#include "ascii_util.h"
#include "string_convert.h"
#include <algorithm>
#include <cmath>
#include <cwchar>
#include <format>
#include <iterator>
#include <limits>

using namespace std::literals;

std::wstring mermaid_util::PageRenderMessage(PageRequest kind, unsigned id, bool dark, float max_width, std::wstring_view locale, std::string_view code_utf8)
{
    static constexpr std::wstring_view kKinds[]{ L"mermaid", L"svg", L"view", L"math" };
    std::wstring msg = std::format(L"{}:{}:{}:{:.2f}:{}:", kKinds[std::to_underlying(kind)], id, dark ? 1 : 0, max_width, locale);
    msg += string_convert::Utf8ToWide(code_utf8);
    return msg;
}

std::wstring mermaid_util::PageCaptureMessage(unsigned id)
{
    return std::format(L"capture:{}:", id);
}

std::wstring mermaid_util::ViewerInitMessage(bool dark, std::span<const std::wstring_view> labels)
{
    std::wstring msg = std::format(L"init:{}:", dark ? 1 : 0);
    for (const std::wstring_view label : labels) {
        msg += label;
        msg += L'\n';
    }
    return msg;
}

std::wstring mermaid_util::ViewerSvgMessage(bool dark, std::wstring_view svg)
{
    std::wstring msg = std::format(L"svg:{}:", dark ? 1 : 0);
    msg += svg;
    return msg;
}

int mermaid_util::QuantizeWidth(float max_width) noexcept
{
    // 細かなペイン幅変動でキャッシュミスを連発させない粒度。
    constexpr int kQuantum = 32;
    if (!(max_width > 0.0f)) {
        return kQuantum;
    }
    return static_cast<int>(std::ceil(max_width / static_cast<float>(kQuantum))) * kQuantum;
}

namespace {

// wstring_view は null 終端が保証されないため、数値部分を切り出してから wcstof に渡す。
float ParseFloatPrefix(std::wstring_view s) noexcept
{
    wchar_t buf[64];
    const auto num_len = std::min(s.size(), std::size(buf) - 1);
    std::char_traits<wchar_t>::copy(buf, s.data(), num_len);
    buf[num_len] = L'\0';
    const float v = std::wcstof(buf, nullptr);
    return std::isfinite(v) ? v : 0.0f;
}

} // namespace

bool mermaid_util::FitsCaptureLimit(float css_width, float css_height, float dpr, unsigned max_side_px) noexcept
{
    const double w = std::ceil(static_cast<double>(css_width) * dpr);
    const double h = std::ceil(static_cast<double>(css_height) * dpr);
    return w <= max_side_px && h <= max_side_px && w * h <= kMaxCapturePixels;
}

mermaid_util::RequestPrefix mermaid_util::ParseRequestPrefix(std::wstring_view body) noexcept
{
    RequestPrefix out;
    if (body.empty() || !ascii_util::IsAsciiDigit(body[0])) {
        return out;
    }
    // wstring_view から直接桁を読み取る。unsigned int に収まらない ID は無効扱いにする。
    constexpr uint64_t kMaxUint = std::numeric_limits<unsigned int>::max();
    uint64_t acc = 0;
    size_t i = 0;
    for (; i < body.size(); ++i) {
        const wchar_t c = body[i];
        if (!ascii_util::IsAsciiDigit(c)) {
            break;
        }
        acc = acc * 10 + static_cast<uint64_t>(c - L'0');
        if (acc > kMaxUint) {
            return out;
        }
    }
    out.valid = true;
    out.id = static_cast<unsigned int>(acc);
    if (i < body.size() && body[i] == L':') {
        out.has_payload = true;
        out.payload = body.substr(i + 1);
    }
    return out;
}

mermaid_util::ParsedWebMessage mermaid_util::ParseWebMessage(std::wstring_view msg) noexcept
{
    ParsedWebMessage out;

    constexpr auto kReadyPrefix = L"mermaid-ready:"sv;
    if (msg.starts_with(kReadyPrefix)) {
        out.kind = WebMessageKind::Ready;
        out.ready_dpr = ParseFloatPrefix(msg.substr(kReadyPrefix.size()));
        return out;
    }
    if (msg == L"mermaid-failed"sv) {
        out.kind = WebMessageKind::Failed;
        return out;
    }
    struct PrefixEntry {
        std::wstring_view prefix;
        WebMessageKind kind;
    };
    constexpr PrefixEntry kEntries[]{
        { L"render-result:"sv, WebMessageKind::RenderResult },
        { L"draw-result:"sv,   WebMessageKind::DrawResult   },
        { L"capture-ready:"sv, WebMessageKind::CaptureReady },
        { L"svg-result:"sv,    WebMessageKind::SvgResult    },
        { L"render-error:"sv,  WebMessageKind::RenderError  },
    };
    for (const auto& e : kEntries) {
        if (msg.starts_with(e.prefix)) {
            out.kind = e.kind;
            out.request = ParseRequestPrefix(msg.substr(e.prefix.size()));
            return out;
        }
    }
    return out;
}

namespace {

// sep で区切った数を先頭から out に読む。足りない分は 0 のまま。
void ParseFloatList(std::wstring_view s, wchar_t sep, std::span<float> out) noexcept
{
    for (float& v : out) {
        const auto pos = s.find(sep);
        v = ParseFloatPrefix(s.substr(0, pos));
        if (pos == std::wstring_view::npos) {
            break;
        }
        s.remove_prefix(pos + 1);
    }
}

} // namespace

mermaid_util::RenderSize mermaid_util::ParseRenderSize(std::wstring_view payload) noexcept
{
    float values[3]{};
    ParseFloatList(payload, L':', values);
    return { values[0], values[1], IsPlausibleDpr(values[2]) ? values[2] : 1.0f };
}

namespace {

// s の先頭から sep までを切り出して進める。sep が無ければ nullopt。
std::optional<std::wstring_view> TakeField(std::wstring_view& s, wchar_t sep = L':') noexcept
{
    const auto pos = s.find(sep);
    if (pos == std::wstring_view::npos) {
        return std::nullopt;
    }
    const auto field = s.substr(0, pos);
    s.remove_prefix(pos + 1);
    return field;
}

// 10 進の数字だけからなる値。max を超えれば nullopt。
std::optional<uint32_t> ParseDecimal(std::wstring_view s, uint32_t max) noexcept
{
    if (s.empty()) {
        return std::nullopt;
    }
    uint64_t v = 0;
    for (const wchar_t c : s) {
        if (c < L'0' || c > L'9') {
            return std::nullopt;
        }
        v = v * 10 + static_cast<uint64_t>(c - L'0');
        if (v > max) {
            return std::nullopt;
        }
    }
    return static_cast<uint32_t>(v);
}

// "<長さ>:<本文>" を切り出して進める。
std::optional<std::wstring_view> TakeText(std::wstring_view& s, uint32_t max_len) noexcept
{
    const auto len_field = TakeField(s);
    const auto len = len_field ? ParseDecimal(*len_field, max_len) : std::nullopt;
    if (!len || *len > s.size()) {
        return std::nullopt;
    }
    const auto text = s.substr(0, *len);
    s.remove_prefix(*len);
    return text;
}

constexpr uint32_t kMaxRgb = 0xFFFFFF;
constexpr uint32_t kMaxFamilyLength = 1024;

std::optional<mermaid_util::DrawingText> ParseDrawingText(std::wstring_view& s)
{
    const auto numbers = TakeField(s);
    if (!numbers || std::ranges::count(*numbers, L',') != 13) {
        return std::nullopt;
    }
    float v[14]{};
    ParseFloatList(*numbers, L',', v);
    const float font_size = v[8];
    const float weight = v[9];
    const float rgb = v[12];
    if (!(font_size > 0.0f && font_size <= 1000.0f) || !(weight >= 1.0f && weight <= 1000.0f) || (v[10] != 0.0f && v[10] != 1.0f) ||
        (v[11] != 0.0f && v[11] != 1.0f) || !(rgb >= 0.0f && rgb <= static_cast<float>(kMaxRgb))) {
        return std::nullopt;
    }
    const auto family = TakeText(s, kMaxFamilyLength);
    const auto text = family ? TakeText(s, std::numeric_limits<uint32_t>::max()) : std::nullopt;
    if (!text) {
        return std::nullopt;
    }
    return mermaid_util::DrawingText{
        .transform = { v[0], v[1], v[2], v[3], v[4], v[5] },
        .x = v[6],
        .y = v[7],
        .font_size = font_size,
        .weight = static_cast<uint16_t>(weight),
        .italic = v[10] != 0.0f,
        .underline = v[11] != 0.0f,
        .rgb = static_cast<uint32_t>(rgb),
        .alpha = std::clamp(v[13], 0.0f, 1.0f),
        .family = std::wstring{ *family },
        .text = std::wstring{ *text },
    };
}

// 数式のラベルの TeX の長さの上限。pele は 1 ラベルの数式を数十個までにするので、長い TeX は偽りとみなす。
constexpr uint32_t kMaxTexLength = 64 * 1024;

std::optional<mermaid_util::DrawingMath> ParseDrawingMath(std::wstring_view& s)
{
    const auto numbers = TakeField(s);
    if (!numbers || std::ranges::count(*numbers, L',') != 10) {
        return std::nullopt;
    }
    float v[11]{};
    ParseFloatList(*numbers, L',', v);
    const float font_size = v[8];
    const float rgb = v[9];
    if (!(font_size > 0.0f && font_size <= 1000.0f) || !(rgb >= 0.0f && rgb <= static_cast<float>(kMaxRgb))) {
        return std::nullopt;
    }
    const auto tex = TakeText(s, kMaxTexLength);
    if (!tex) {
        return std::nullopt;
    }
    return mermaid_util::DrawingMath{
        .transform = { v[0], v[1], v[2], v[3], v[4], v[5] },
        .x = v[6],
        .y = v[7],
        .font_size = font_size,
        .rgb = static_cast<uint32_t>(rgb),
        .alpha = std::clamp(v[10], 0.0f, 1.0f),
        .tex = string_convert::WideToUtf8(*tex),
    };
}

} // namespace

namespace {

// ParseDrawing は長さを UTF-16 の単位で読む。
size_t Utf16Length(std::string_view utf8) noexcept
{
    size_t n = 0;
    for (const unsigned char c : utf8) {
        n += (c & 0xC0) != 0x80; // 継続バイト以外で 1 文字
        n += c >= 0xF0;          // 4 バイトの文字はサロゲートの 2 単位
    }
    return n;
}

} // namespace

std::string mermaid_util::SerializeDrawing(const DiagramDrawing& drawing)
{
    std::string out;
    const auto out_it = std::back_inserter(out);
    // {} の浮動小数は最短の往復表記なので、wcstof で同じ値に読み戻せる。
    std::format_to(out_it, "{}:{}:{}:{}:{}:", drawing.size.width, drawing.size.height, drawing.size.dpr, drawing.background, drawing.layers.size());
    for (const auto& layer : drawing.layers) {
        std::format_to(out_it, "{}:{}{}:", Utf16Length(layer.shapes_svg), layer.shapes_svg, layer.texts.size());
        for (const auto& t : layer.texts) {
            const auto& m = t.transform;
            std::format_to(out_it, "{},{},{},{},{},{},{},{},{},{},{},{},{},{}:", m[0], m[1], m[2], m[3], m[4], m[5], t.x, t.y, t.font_size, t.weight,
                           t.italic ? 1 : 0, t.underline ? 1 : 0, t.rgb, t.alpha);
            std::format_to(out_it, "{}:{}{}:{}", t.family.size(), string_convert::WideToUtf8(t.family), t.text.size(), string_convert::WideToUtf8(t.text));
        }
        std::format_to(out_it, "{}:", layer.maths.size());
        for (const auto& m : layer.maths) {
            const auto& t = m.transform;
            std::format_to(out_it, "{},{},{},{},{},{},{},{},{},{},{}:{}:{}", t[0], t[1], t[2], t[3], t[4], t[5], m.x, m.y, m.font_size, m.rgb, m.alpha,
                           Utf16Length(m.tex), m.tex);
        }
    }
    return out;
}

size_t mermaid_util::DrawingBytes(const DiagramDrawing& drawing) noexcept
{
    size_t bytes = sizeof(drawing);
    for (const auto& layer : drawing.layers) {
        bytes += sizeof(layer) + layer.shapes_svg.size();
        for (const auto& t : layer.texts) {
            bytes += sizeof(t) + (t.family.size() + t.text.size()) * sizeof(wchar_t);
        }
        // 数式の描画命令はおおよそ 1 つ 1KB (字形の並び 10 本ほど)。
        for (const auto& m : layer.maths) {
            bytes += sizeof(m) + m.tex.size() + (m.drawing ? 1024 : 0);
        }
    }
    return bytes;
}

std::optional<mermaid_util::DiagramDrawing> mermaid_util::ParseDrawing(std::wstring_view payload)
{
    // 先頭の "<幅>:<高さ>:<dpr>" は RenderResult と同じ。
    std::wstring_view s = payload;
    for (int i = 0; i < 3; ++i) {
        if (!TakeField(s)) {
            return std::nullopt;
        }
    }
    const RenderSize size = ParseRenderSize(payload.substr(0, payload.size() - s.size()));
    // 数だけ大きく偽って確保させないよう、数の上限は残りの長さから決める (層は少なくとも 4 文字、並びは 30 文字ある)。
    const auto take_count = [&s](uint32_t min_chars) {
        const auto field = TakeField(s);
        return field ? ParseDecimal(*field, static_cast<uint32_t>(std::min<size_t>(s.size() / min_chars, std::numeric_limits<uint32_t>::max())))
                     : std::nullopt;
    };
    const auto background_field = TakeField(s);
    const auto background = background_field ? ParseDecimal(*background_field, kMaxRgb) : std::nullopt;
    const auto layer_count = background ? take_count(4) : std::nullopt;
    if (!layer_count || !(size.width > 0.0f) || !(size.height > 0.0f)) {
        return std::nullopt;
    }
    DiagramDrawing out{
        .size = size,
        .background = *background,
    };
    out.layers.reserve(*layer_count);
    for (uint32_t i = 0; i < *layer_count; ++i) {
        const auto svg = TakeText(s, std::numeric_limits<uint32_t>::max());
        const auto text_count = svg ? take_count(30) : std::nullopt;
        if (!text_count) {
            return std::nullopt;
        }
        auto& layer = out.layers.emplace_back(DrawingLayer{ .shapes_svg = string_convert::WideToUtf8(*svg) });
        layer.texts.reserve(*text_count);
        for (uint32_t k = 0; k < *text_count; ++k) {
            auto text = ParseDrawingText(s);
            if (!text) {
                return std::nullopt;
            }
            layer.texts.push_back(std::move(*text));
        }
        const auto math_count = take_count(16);
        if (!math_count) {
            return std::nullopt;
        }
        layer.maths.reserve(*math_count);
        for (uint32_t k = 0; k < *math_count; ++k) {
            auto math = ParseDrawingMath(s);
            if (!math) {
                return std::nullopt;
            }
            layer.maths.push_back(std::move(*math));
        }
    }
    if (!s.empty()) {
        return std::nullopt;
    }
    return out;
}

std::wstring mermaid_util::SanitizeErrorMessage(std::wstring_view msg, size_t max_len)
{
    std::wstring result;
    result.reserve(std::min(msg.size(), max_len));
    bool prev_space = true; // 先頭の空白は落とす
    bool truncated = false;
    for (size_t i = 0; i < msg.size(); ++i) {
        const wchar_t c = msg[i];
        // 改行・タブに加え、ページから届いた制御文字 (豆腐表示になる) も空白に潰す。
        if (c <= L' ') {
            if (!prev_space) {
                result += L' ';
                prev_space = true;
            }
            continue;
        }
        prev_space = false;
        result += c;
        if (result.size() >= max_len) {
            // 残りが空白のみなら実質切り詰め無しなので省略記号を付けない。
            const auto rest = msg.substr(i + 1);
            truncated = std::ranges::any_of(rest, [](wchar_t r) static noexcept { return r > L' '; });
            break;
        }
    }
    while (!result.empty() && result.back() == L' ') {
        result.pop_back();
    }
    if (truncated && !result.empty()) {
        result.back() = L'…';
    }
    return result;
}

uint64_t mermaid_util::DiagramCodeHash(const Node& node) noexcept
{
    // 描き方を変えたときは MermaidFileCache::VERSION を上げ、古い描き方の PNG をフォルダごと捨てる。
    return Fnv1a64(node.GetText());
}

uint64_t mermaid_util::EnsureDiagramCodeHash(uint64_t& cached, const Node& node) noexcept
{
    if (cached == 0) {
        cached = DiagramCodeHash(node);
    }
    return cached;
}

namespace {
// 幅と dark_mode は別 prime で xor mix する。xor なのでコード側のソルトと順序を問わない。
// ディスクキャッシュのファイル名になるので値を変えない。
constexpr uint64_t MixDiagramKey(uint64_t code_hash, uint64_t width_term, bool dark_mode) noexcept
{
    return code_hash ^ (width_term * kFnv1a64Prime) ^ (static_cast<uint64_t>(dark_mode) * 2654435761ULL); // Knuth multiplicative
}
} // namespace

uint64_t mermaid_util::DiagramRequestHash(uint64_t code_hash, float max_width, bool dark_mode) noexcept
{
    return MixDiagramKey(code_hash, static_cast<uint64_t>(QuantizeWidth(max_width)), dark_mode);
}

// 量子化幅は 32 の倍数なので、幅 1 として混ぜれば幅つきのキーと重ならない。
uint64_t mermaid_util::DiagramWidthFreeHash(uint64_t code_hash, bool dark_mode) noexcept
{
    return MixDiagramKey(code_hash, 1, dark_mode);
}

uint64_t mermaid_util::NodeDiagramHash(const Node& node, float max_width, bool dark_mode) noexcept
{
    return DiagramRequestHash(DiagramCodeHash(node), max_width, dark_mode);
}
