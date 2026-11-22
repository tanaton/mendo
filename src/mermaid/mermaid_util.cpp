#include "mermaid_util.h"
#include "document_types.h"
#include "utility.h"
#include <algorithm>
#include <cmath>
#include <type_traits>

int mermaid_util::QuantizeWidth(float max_width) noexcept
{
    // 細かなペイン幅変動でキャッシュミスを連発させない粒度。
    constexpr int kQuantum = 32;
    if (!(max_width > 0.0f)) {
        return kQuantum;
    }
    return static_cast<int>(std::ceil(max_width / static_cast<float>(kQuantum))) * kQuantum;
}

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

bool mermaid_util::IsDrawable(const DrawingPlacement& p) noexcept
{
    constexpr uint32_t kMaxRgb = 0xFFFFFF;
    return std::ranges::all_of(p.transform, [](float v) static { return std::isfinite(v); }) && std::isfinite(p.x) && std::isfinite(p.y) &&
           p.font_size > 0.0f && p.font_size <= kMaxFontSize && p.rgb <= kMaxRgb && p.alpha >= 0.0f && p.alpha <= 1.0f;
}

bool mermaid_util::IsDrawableSize(float width, float height) noexcept
{
    return width > 0.0f && height > 0.0f && std::isfinite(width) && std::isfinite(height);
}

mermaid_util::DrawingChars mermaid_util::AppendChars(std::wstring& chars, std::wstring_view s)
{
    const DrawingChars range{ static_cast<uint32_t>(chars.size()), static_cast<uint32_t>(s.size()) };
    chars.append(s);
    return range;
}

size_t mermaid_util::DrawingBytes(const DiagramDrawing& drawing) noexcept
{
    size_t bytes = sizeof(drawing) + drawing.defs.size() + drawing.chars.size() * sizeof(wchar_t) + drawing.layers.size() * sizeof(DrawingLayer);
    for (const auto& layer : drawing.layers) {
        bytes += layer.shapes.size() + layer.texts.size() * sizeof(DrawingText) + layer.maths.size() * sizeof(DrawingMath);
        for (const auto& math : layer.maths) {
            bytes += math.tex.size();
        }
    }
    return bytes;
}

uint64_t mermaid_util::DrawingHash(const DiagramDrawing& drawing) noexcept
{
    // 詰め物が無いのでバイト列のまま混ぜられる。
    static_assert(sizeof(DrawingPlacement) == 11 * 4);
    static_assert(sizeof(DrawingText) == sizeof(DrawingPlacement) + 4 + 2 * sizeof(DrawingChars));
    static_assert(std::is_trivially_copyable_v<DrawingText>);
    uint64_t hash = 0;
    const auto mix = [&hash](const void* data, size_t size) noexcept { hash = rapidhash_withSeed(data, size, hash); };
    mix(&drawing.width, sizeof(drawing.width));
    mix(&drawing.height, sizeof(drawing.height));
    mix(&drawing.background, sizeof(drawing.background));
    mix(drawing.defs.data(), drawing.defs.size());
    mix(drawing.chars.data(), drawing.chars.size() * sizeof(wchar_t));
    for (const auto& layer : drawing.layers) {
        mix(layer.shapes.data(), layer.shapes.size());
        mix(layer.texts.data(), layer.texts.size() * sizeof(DrawingText));
        for (const auto& math : layer.maths) {
            mix(&math.at, sizeof(math.at));
            mix(math.tex.data(), math.tex.size());
        }
    }
    return hash;
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
