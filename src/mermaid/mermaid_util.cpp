#include "mermaid_util.h"
#include "document_types.h"
#include "utility.h"
#include "string_convert.h"
#include <algorithm>
#include <charconv>
#include <cmath>
#include <format>
#include <iterator>
#include <limits>
#include <span>

using namespace std::literals;

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

// 数として読めない値と float に収まらない値は NaN にする。有限でない値もそのまま返し、範囲は呼ぶ側が確かめる。
float ParseFloatPrefix(std::string_view s) noexcept
{
    float v = 0.0f;
    const auto [ptr, ec] = std::from_chars(s.data(), s.data() + s.size(), v);
    return ec == std::errc{} ? v : std::numeric_limits<float>::quiet_NaN();
}

// カンマで区切った数を先頭から out に読む。足りない分は 0 のまま。
void ParseFloatList(std::string_view s, std::span<float> out) noexcept
{
    for (float& v : out) {
        const auto pos = s.find(',');
        v = ParseFloatPrefix(s.substr(0, pos));
        if (pos == std::string_view::npos) {
            break;
        }
        s.remove_prefix(pos + 1);
    }
}

// s の先頭から ':' までを切り出して進める。':' が無ければ nullopt。
std::optional<std::string_view> TakeField(std::string_view& s) noexcept
{
    const auto pos = s.find(':');
    if (pos == std::string_view::npos) {
        return std::nullopt;
    }
    const auto field = s.substr(0, pos);
    s.remove_prefix(pos + 1);
    return field;
}

// 10 進の数字だけからなる値。max を超えれば nullopt。
std::optional<uint32_t> ParseDecimal(std::string_view s, uint32_t max) noexcept
{
    if (s.empty()) {
        return std::nullopt;
    }
    uint64_t v = 0;
    for (const char c : s) {
        if (c < '0' || c > '9') {
            return std::nullopt;
        }
        v = v * 10 + static_cast<uint64_t>(c - '0');
        if (v > max) {
            return std::nullopt;
        }
    }
    return static_cast<uint32_t>(v);
}

// "<長さ>:<本文>" を切り出して進める。長さは残りを超えられないので、偽った長さで大きく確保することはない。
std::optional<std::string_view> TakeText(std::string_view& s) noexcept
{
    const auto len_field = TakeField(s);
    const auto len = len_field ? ParseDecimal(*len_field, std::numeric_limits<uint32_t>::max()) : std::nullopt;
    if (!len || *len > s.size()) {
        return std::nullopt;
    }
    const auto text = s.substr(0, *len);
    s.remove_prefix(*len);
    return text;
}

constexpr uint32_t kMaxRgb = 0xFFFFFF;

// 置き方の数 (変換 6・x・y・大きさ・色・不透明度)。文字と数式の数の並びの先頭に書く。
constexpr size_t kPlacementNumbers = 11;

// 数の並びのうち先頭 kPlacementNumbers 個を読む。書けない置き方 (IsStorable) なら nullopt。
std::optional<mermaid_util::DrawingPlacement> ToPlacement(std::span<const float> v) noexcept
{
    const float rgb = v[9];
    if (!(rgb >= 0.0f && rgb <= static_cast<float>(kMaxRgb))) {
        return std::nullopt;
    }
    const mermaid_util::DrawingPlacement p{
        .transform = { v[0], v[1], v[2], v[3], v[4], v[5] },
        .x = v[6],
        .y = v[7],
        .font_size = v[8],
        .rgb = static_cast<uint32_t>(rgb),
        .alpha = v[10],
    };
    return mermaid_util::IsStorable(p) ? std::optional{ p } : std::nullopt;
}

template <class Out>
void AppendPlacement(Out out, const mermaid_util::DrawingPlacement& p)
{
    const auto& m = p.transform;
    std::format_to(out, "{},{},{},{},{},{},{},{},{},{},{}", m[0], m[1], m[2], m[3], m[4], m[5], p.x, p.y, p.font_size, p.rgb, p.alpha);
}

std::optional<mermaid_util::DrawingText> ParseDrawingText(std::string_view& s)
{
    const auto numbers = TakeField(s);
    if (!numbers || std::ranges::count(*numbers, ',') != kPlacementNumbers + 2) {
        return std::nullopt;
    }
    float v[kPlacementNumbers + 3]{};
    ParseFloatList(*numbers, v);
    const auto at = ToPlacement(v);
    const float weight = v[11];
    if (!at || !(weight >= 1.0f && weight <= 1000.0f) || (v[12] != 0.0f && v[12] != 1.0f) || !(v[13] >= 0.0f && v[13] <= mermaid_util::kTextDecorationBits) ||
        v[13] != std::floor(v[13])) {
        return std::nullopt;
    }
    const auto family = TakeText(s);
    const auto text = family ? TakeText(s) : std::nullopt;
    if (!text) {
        return std::nullopt;
    }
    return mermaid_util::DrawingText{
        .at = *at,
        .weight = static_cast<uint16_t>(weight),
        .italic = v[12] != 0.0f,
        .decoration = static_cast<uint8_t>(v[13]),
        .family = string_convert::Utf8ToWide(*family),
        .text = string_convert::Utf8ToWide(*text),
    };
}

std::optional<mermaid_util::DrawingMath> ParseDrawingMath(std::string_view& s)
{
    const auto numbers = TakeField(s);
    if (!numbers || std::ranges::count(*numbers, ',') != kPlacementNumbers - 1) {
        return std::nullopt;
    }
    float v[kPlacementNumbers]{};
    ParseFloatList(*numbers, v);
    const auto at = ToPlacement(v);
    const auto tex = at ? TakeText(s) : std::nullopt;
    if (!tex) {
        return std::nullopt;
    }
    return mermaid_util::DrawingMath{ .at = *at, .tex = std::string{ *tex } };
}

} // namespace

bool mermaid_util::IsFlowchartCode(std::string_view code) noexcept
{
    bool in_frontmatter = false;
    bool first_line = true;
    while (!code.empty()) {
        const size_t eol = code.find('\n');
        std::string_view line = code.substr(0, eol);
        code = eol == std::string_view::npos ? std::string_view{} : code.substr(eol + 1);
        while (!line.empty() && (line.front() == ' ' || line.front() == '\t')) {
            line.remove_prefix(1);
        }
        while (!line.empty() && (line.back() == ' ' || line.back() == '\t' || line.back() == '\r')) {
            line.remove_suffix(1);
        }
        const bool fence = line == "---";
        if ((first_line && fence) || (in_frontmatter && fence)) {
            in_frontmatter = !in_frontmatter;
            first_line = false;
            continue;
        }
        first_line = false;
        if (in_frontmatter || line.empty() || line.starts_with("%%")) {
            continue;
        }
        const std::string_view keyword = line.substr(0, line.find_first_of(" \t;"));
        return keyword == "flowchart" || keyword == "graph" || keyword == "flowchart-elk";
    }
    return false;
}

bool mermaid_util::IsStorable(const DrawingPlacement& p) noexcept
{
    return std::ranges::all_of(p.transform, [](float v) static { return std::isfinite(v); }) && std::isfinite(p.x) && std::isfinite(p.y) &&
           p.font_size > 0.0f && p.font_size <= kMaxFontSize && p.rgb <= kMaxRgb && p.alpha >= 0.0f && p.alpha <= 1.0f;
}

std::string mermaid_util::SerializeDrawing(const DiagramDrawing& drawing)
{
    std::string out;
    const auto out_it = std::back_inserter(out);
    // {} の浮動小数は最短の往復表記なので、from_chars で同じ値に読み戻せる。長さは UTF-8 のバイト数。
    std::format_to(out_it, "{}:{}:{}:{}:{}{}:", drawing.width, drawing.height, drawing.background, drawing.defs.size(), drawing.defs,
                   drawing.layers.size());
    for (const auto& layer : drawing.layers) {
        std::format_to(out_it, "{}:{}{}:", layer.shapes.size(), layer.shapes, layer.texts.size());
        for (const auto& t : layer.texts) {
            AppendPlacement(out_it, t.at);
            std::format_to(out_it, ",{},{},{}:", t.weight, t.italic ? 1 : 0, t.decoration);
            const std::string family = string_convert::WideToUtf8(t.family);
            const std::string text = string_convert::WideToUtf8(t.text);
            std::format_to(out_it, "{}:{}{}:{}", family.size(), family, text.size(), text);
        }
        std::format_to(out_it, "{}:", layer.maths.size());
        for (const auto& m : layer.maths) {
            AppendPlacement(out_it, m.at);
            std::format_to(out_it, ":{}:{}", m.tex.size(), m.tex);
        }
    }
    return out;
}

mermaid_util::DiagramSize mermaid_util::FittedDiagramSize(const DiagramDrawing& drawing, float max_width) noexcept
{
    float w = drawing.width;
    float h = drawing.height;
    if (!(max_width > 0.0f && w > max_width)) {
        return { w, h };
    }
    h = h * max_width / w;
    w = max_width;
    return { std::ceil(std::floor(w * 64.0f) / 64.0f), std::ceil(std::floor(h * 64.0f) / 64.0f) };
}

std::optional<mermaid_util::DiagramDrawing> mermaid_util::ParseDrawing(std::string_view payload)
{
    std::string_view s = payload;
    const auto width_field = TakeField(s);
    const auto height_field = width_field ? TakeField(s) : std::nullopt;
    if (!height_field) {
        return std::nullopt;
    }
    const float width = ParseFloatPrefix(*width_field);
    const float height = ParseFloatPrefix(*height_field);
    // 数だけ大きく偽って確保させないよう、数の上限は残りの長さから決める (層は少なくとも 4 文字、並びは 30 文字ある)。
    const auto take_count = [&s](uint32_t min_chars) {
        const auto field = TakeField(s);
        return field ? ParseDecimal(*field, static_cast<uint32_t>(std::min<size_t>(s.size() / min_chars, std::numeric_limits<uint32_t>::max())))
                     : std::nullopt;
    };
    const auto background_field = TakeField(s);
    const auto background = background_field ? ParseDecimal(*background_field, kMaxRgb) : std::nullopt;
    const auto defs = background ? TakeText(s) : std::nullopt;
    const auto layer_count = defs ? take_count(4) : std::nullopt;
    if (!layer_count || !(width > 0.0f && std::isfinite(width)) || !(height > 0.0f && std::isfinite(height))) {
        return std::nullopt;
    }
    DiagramDrawing out{
        .width = width,
        .height = height,
        .background = *background,
        .defs = std::string{ *defs },
    };
    out.layers.reserve(*layer_count);
    for (uint32_t i = 0; i < *layer_count; ++i) {
        const auto svg = TakeText(s);
        const auto text_count = svg ? take_count(30) : std::nullopt;
        if (!text_count) {
            return std::nullopt;
        }
        auto& layer = out.layers.emplace_back(DrawingLayer{ .shapes = std::string{ *svg } });
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
        // 改行・タブに加え、pele の文言に入る制御文字 (豆腐表示になる) も空白に潰す。
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
    return DiagramCodeHash(node.GetText());
}

uint64_t mermaid_util::DiagramCodeHash(std::string_view code) noexcept
{
    return RapidHash(code);
}

namespace {
// 幅と dark_mode は別の係数で xor mix する。
constexpr uint64_t MixDiagramKey(uint64_t code_hash, uint64_t width_term, bool dark_mode) noexcept
{
    return code_hash ^ (width_term * kGoldenRatio64) ^ (static_cast<uint64_t>(dark_mode) * 2654435761ULL); // Knuth multiplicative
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
