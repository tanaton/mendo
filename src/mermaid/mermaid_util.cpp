#include "mermaid_util.h"
#include "document_types.h"
#include "utility.h"
#include "string_convert.h"
#include <algorithm>
#include <cmath>
#include <cwchar>
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

// 置き方の数 (変換 6・x・y・大きさ・色・不透明度)。文字と数式の数の並びの先頭に書く。
constexpr size_t kPlacementNumbers = 11;

// 数の並びのうち先頭 kPlacementNumbers 個を読む。大きさと色が範囲の外なら nullopt。
std::optional<mermaid_util::DrawingPlacement> ToPlacement(std::span<const float> v) noexcept
{
    const float font_size = v[8];
    const float rgb = v[9];
    if (!(font_size > 0.0f && font_size <= 1000.0f) || !(rgb >= 0.0f && rgb <= static_cast<float>(kMaxRgb))) {
        return std::nullopt;
    }
    return mermaid_util::DrawingPlacement{
        .transform = { v[0], v[1], v[2], v[3], v[4], v[5] },
        .x = v[6],
        .y = v[7],
        .font_size = font_size,
        .rgb = static_cast<uint32_t>(rgb),
        .alpha = std::clamp(v[10], 0.0f, 1.0f),
    };
}

template <class Out>
void AppendPlacement(Out out, const mermaid_util::DrawingPlacement& p)
{
    const auto& m = p.transform;
    std::format_to(out, "{},{},{},{},{},{},{},{},{},{},{}", m[0], m[1], m[2], m[3], m[4], m[5], p.x, p.y, p.font_size, p.rgb, p.alpha);
}

std::optional<mermaid_util::DrawingText> ParseDrawingText(std::wstring_view& s)
{
    const auto numbers = TakeField(s);
    if (!numbers || std::ranges::count(*numbers, L',') != kPlacementNumbers + 2) {
        return std::nullopt;
    }
    float v[kPlacementNumbers + 3]{};
    ParseFloatList(*numbers, L',', v);
    const auto at = ToPlacement(v);
    const float weight = v[11];
    if (!at || !(weight >= 1.0f && weight <= 1000.0f) || (v[12] != 0.0f && v[12] != 1.0f) || (v[13] != 0.0f && v[13] != 1.0f)) {
        return std::nullopt;
    }
    const auto family = TakeText(s, kMaxFamilyLength);
    const auto text = family ? TakeText(s, std::numeric_limits<uint32_t>::max()) : std::nullopt;
    if (!text) {
        return std::nullopt;
    }
    return mermaid_util::DrawingText{
        .at = *at,
        .weight = static_cast<uint16_t>(weight),
        .italic = v[12] != 0.0f,
        .underline = v[13] != 0.0f,
        .family = std::wstring{ *family },
        .text = std::wstring{ *text },
    };
}

// 数式のラベルの TeX の長さの上限。pele は 1 ラベルの数式を数十個までにするので、長い TeX は偽りとみなす。
constexpr uint32_t kMaxTexLength = 64 * 1024;

std::optional<mermaid_util::DrawingMath> ParseDrawingMath(std::wstring_view& s)
{
    const auto numbers = TakeField(s);
    if (!numbers || std::ranges::count(*numbers, L',') != kPlacementNumbers - 1) {
        return std::nullopt;
    }
    float v[kPlacementNumbers]{};
    ParseFloatList(*numbers, L',', v);
    const auto at = ToPlacement(v);
    const auto tex = at ? TakeText(s, kMaxTexLength) : std::nullopt;
    if (!tex) {
        return std::nullopt;
    }
    return mermaid_util::DrawingMath{ .at = *at, .tex = string_convert::WideToUtf8(*tex) };
}

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
    std::format_to(out_it, "{}:{}:{}:{}:{}{}:", drawing.width, drawing.height, drawing.background, Utf16Length(drawing.defs), drawing.defs,
                   drawing.layers.size());
    for (const auto& layer : drawing.layers) {
        std::format_to(out_it, "{}:{}{}:", Utf16Length(layer.shapes), layer.shapes, layer.texts.size());
        for (const auto& t : layer.texts) {
            AppendPlacement(out_it, t.at);
            std::format_to(out_it, ",{},{},{}:", t.weight, t.italic ? 1 : 0, t.underline ? 1 : 0);
            std::format_to(out_it, "{}:{}{}:{}", t.family.size(), string_convert::WideToUtf8(t.family), t.text.size(), string_convert::WideToUtf8(t.text));
        }
        std::format_to(out_it, "{}:", layer.maths.size());
        for (const auto& m : layer.maths) {
            AppendPlacement(out_it, m.at);
            std::format_to(out_it, ":{}:{}", Utf16Length(m.tex), m.tex);
        }
    }
    return out;
}

size_t mermaid_util::DrawingBytes(const DiagramDrawing& drawing) noexcept
{
    size_t bytes = sizeof(drawing) + drawing.defs.size();
    for (const auto& layer : drawing.layers) {
        bytes += sizeof(layer) + layer.shapes.size();
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
    std::wstring_view s = payload;
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
    const auto defs = background ? TakeText(s, std::numeric_limits<uint32_t>::max()) : std::nullopt;
    const auto layer_count = defs ? take_count(4) : std::nullopt;
    if (!layer_count || !(width > 0.0f) || !(height > 0.0f)) {
        return std::nullopt;
    }
    DiagramDrawing out{
        .width = width,
        .height = height,
        .background = *background,
        .defs = string_convert::WideToUtf8(*defs),
    };
    out.layers.reserve(*layer_count);
    for (uint32_t i = 0; i < *layer_count; ++i) {
        const auto svg = TakeText(s, std::numeric_limits<uint32_t>::max());
        const auto text_count = svg ? take_count(30) : std::nullopt;
        if (!text_count) {
            return std::nullopt;
        }
        auto& layer = out.layers.emplace_back(DrawingLayer{ .shapes = string_convert::WideToUtf8(*svg) });
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
    // 描き方を変えたときは MermaidFileCache::VERSION を上げ、古い描き方のファイルをフォルダごと捨てる。
    return Fnv1a64(code);
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
