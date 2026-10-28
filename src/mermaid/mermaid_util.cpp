#include "mermaid_util.h"
#include "document_types.h"
#include "utility.h"
#include "syntax.h"
#include "ascii_util.h"
#include <algorithm>
#include <bit>
#include <cmath>
#include <cwchar>
#include <format>
#include <iterator>
#include <limits>

using namespace std::literals;

std::wstring mermaid_util::JsEscape(std::wstring_view input)
{
    std::wstring result;
    result.reserve(input.size() + input.size() / 4);
    for (wchar_t c : input) {
        switch (c) {
        case L'\\':
            result += L"\\\\";
            break;
        case L'\'':
            result += L"\\'";
            break;
        case L'"':
            result += L"\\\"";
            break;
        case L'\n':
            result += L"\\n";
            break;
        case L'\r':
            result += L"\\r";
            break;
        case L'\t':
            result += L"\\t";
            break;
        case L'`':
            result += L"\\`";
            break;
        case L'$':
            result += L"\\$";
            break;
        default:
            if (c < 0x20 || c == 0x2028 || c == 0x2029) {
                // 制御文字およびJS文字列リテラルで特殊なU+2028/U+2029をエスケープ
                std::format_to(std::back_inserter(result), L"\\u{:04x}", static_cast<unsigned>(c));
            }
            else {
                result += c;
            }
            break;
        }
    }
    return result;
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
    constexpr double kMaxPixels = 64.0 * 1024 * 1024;
    const double w = std::ceil(static_cast<double>(css_width) * dpr);
    const double h = std::ceil(static_cast<double>(css_height) * dpr);
    return w <= max_side_px && h <= max_side_px && w * h <= kMaxPixels;
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
        { L"capture-ready:"sv, WebMessageKind::CaptureReady },
        { L"svg-result:"sv,    WebMessageKind::SvgResult    },
        { L"render-error:"sv,  WebMessageKind::RenderError  },
        { L"math-result:"sv,   WebMessageKind::MathResult   },
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

mermaid_util::RenderSize mermaid_util::ParseRenderSize(std::wstring_view payload) noexcept
{
    float values[3]{};
    for (float& v : values) {
        const auto colon = payload.find(L':');
        v = ParseFloatPrefix(payload.substr(0, colon));
        if (colon == std::wstring_view::npos) {
            break;
        }
        payload.remove_prefix(colon + 1);
    }
    return { values[0], values[1], values[2] };
}

namespace {

// ページの外を指す箱で、型の外のメモリを読ませない。幅 0 は描けなかった数式。
bool IsValidMathBox(const mermaid_util::MathBox& box, const mermaid_util::RenderSize& page) noexcept
{
    if (box.width == 0.0f) {
        return box.x == 0.0f && box.y == 0.0f && box.height == 0.0f && box.baseline == 0.0f;
    }
    return box.x >= 0.0f && box.y >= 0.0f && box.width > 0.0f && box.height > 0.0f &&
           box.x + box.width <= page.width + 1.0f && box.y + box.height <= page.height + 1.0f &&
           box.baseline >= 0.0f && box.baseline <= box.height + 1.0f;
}

constexpr uint8_t kMathBoxesChunkType[4]{ 'm', 'd', 'M', 'B' };
constexpr uint32_t kMathBoxesChunkVersion = 1;
constexpr uint8_t kPngSignature[8]{ 0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n' };

uint32_t Crc32(std::span<const uint8_t> a, std::span<const uint8_t> b = {}) noexcept
{
    uint32_t crc = 0xFFFFFFFFu;
    for (const auto part : { a, b }) {
        for (const uint8_t byte : part) {
            crc ^= byte;
            for (int k = 0; k < 8; ++k) {
                crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
            }
        }
    }
    return ~crc;
}

void PutBe32(std::vector<uint8_t>& out, uint32_t v)
{
    for (int shift = 24; shift >= 0; shift -= 8) {
        out.push_back(static_cast<uint8_t>(v >> shift));
    }
}

uint32_t GetBe32(const uint8_t* p) noexcept
{
    return (uint32_t{ p[0] } << 24) | (uint32_t{ p[1] } << 16) | (uint32_t{ p[2] } << 8) | p[3];
}

} // namespace

std::vector<uint8_t> mermaid_util::AppendMathBoxesChunk(std::span<const uint8_t> png, const MathBatchLayout& layout)
{
    // IEND (長さ 0 + 種類 + CRC の 12 バイト) の直前に差し込む。
    constexpr size_t kIendSize = 12;
    if (png.size() < sizeof(kPngSignature) + kIendSize || !std::ranges::equal(png.first(sizeof(kPngSignature)), kPngSignature) ||
        !std::ranges::equal(png.last(kIendSize).subspan(4, 4), std::span<const uint8_t>{ reinterpret_cast<const uint8_t*>("IEND"), 4 })) {
        return {};
    }
    std::vector<uint8_t> data;
    PutBe32(data, kMathBoxesChunkVersion);
    const auto put_float = [&data](float v) { PutBe32(data, std::bit_cast<uint32_t>(v)); };
    put_float(layout.size.width);
    put_float(layout.size.height);
    put_float(layout.size.dpr);
    PutBe32(data, static_cast<uint32_t>(layout.boxes.size()));
    for (const auto& b : layout.boxes) {
        for (const float v : { b.x, b.y, b.width, b.height, b.baseline }) {
            put_float(v);
        }
    }
    std::vector<uint8_t> out(png.begin(), png.end() - kIendSize);
    out.reserve(png.size() + data.size() + 12);
    PutBe32(out, static_cast<uint32_t>(data.size()));
    out.insert(out.end(), std::begin(kMathBoxesChunkType), std::end(kMathBoxesChunkType));
    out.insert(out.end(), data.begin(), data.end());
    PutBe32(out, Crc32(kMathBoxesChunkType, data));
    out.insert(out.end(), png.end() - kIendSize, png.end());
    return out;
}

std::optional<mermaid_util::MathBatchLayout> mermaid_util::ReadMathBoxesChunk(std::span<const uint8_t> png, size_t expected)
{
    if (png.size() < sizeof(kPngSignature) || !std::ranges::equal(png.first(sizeof(kPngSignature)), kPngSignature)) {
        return std::nullopt;
    }
    for (size_t pos = sizeof(kPngSignature); pos + 12 <= png.size();) {
        const uint32_t length = GetBe32(&png[pos]);
        if (length > png.size() - pos - 12) {
            return std::nullopt;
        }
        const auto type = png.subspan(pos + 4, 4);
        const auto data = png.subspan(pos + 8, length);
        if (!std::ranges::equal(type, kMathBoxesChunkType)) {
            pos += size_t{ 12 } + length;
            continue;
        }
        if (GetBe32(&png[pos + 8 + length]) != Crc32(type, data) || data.size() < 20) {
            return std::nullopt;
        }
        const auto get_float = [&data](size_t i) { return std::bit_cast<float>(GetBe32(&data[i * 4])); };
        if (GetBe32(&data[0]) != kMathBoxesChunkVersion || GetBe32(&data[16]) != expected || data.size() != 20 + expected * 20) {
            return std::nullopt;
        }
        MathBatchLayout out{ .size = { get_float(1), get_float(2), get_float(3) } };
        if (!(out.size.width > 0.0f && out.size.height > 0.0f && IsPlausibleDpr(out.size.dpr))) {
            return std::nullopt;
        }
        out.boxes.reserve(expected);
        for (size_t i = 0; i < expected; ++i) {
            const size_t f = 5 + i * 5;
            const MathBox box{ get_float(f), get_float(f + 1), get_float(f + 2), get_float(f + 3), get_float(f + 4) };
            if (!IsValidMathBox(box, out.size)) {
                return std::nullopt;
            }
            out.boxes.push_back(box);
        }
        return out;
    }
    return std::nullopt;
}

std::optional<mermaid_util::MathBatchLayout> mermaid_util::ParseMathBatch(std::wstring_view payload, size_t expected)
{
    const auto bar = payload.find(L'|');
    if (bar == std::wstring_view::npos) {
        return std::nullopt;
    }
    MathBatchLayout out{ .size = ParseRenderSize(payload.substr(0, bar)) };
    if (out.size.width <= 0.0f || out.size.height <= 0.0f) {
        return std::nullopt;
    }
    out.boxes.reserve(expected);
    std::wstring_view rest = payload.substr(bar + 1);
    while (true) {
        // 要求より多い。
        if (out.boxes.size() == expected) {
            return std::nullopt;
        }
        const auto semi = rest.find(L';');
        const auto item = rest.substr(0, semi);
        MathBox box{};
        if (item != L"e"sv) {
            float* const fields[]{ &box.x, &box.y, &box.width, &box.height, &box.baseline };
            std::wstring_view f = item;
            for (float* v : fields) {
                const auto comma = f.find(L',');
                *v = ParseFloatPrefix(f.substr(0, comma));
                f = comma == std::wstring_view::npos ? std::wstring_view{} : f.substr(comma + 1);
            }
            if (box.width <= 0.0f || !IsValidMathBox(box, out.size)) {
                return std::nullopt;
            }
        }
        out.boxes.push_back(box);
        if (semi == std::wstring_view::npos) {
            break;
        }
        rest.remove_prefix(semi + 1);
    }
    if (out.boxes.size() != expected) {
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
            truncated = std::ranges::any_of(rest, [](wchar_t r) static { return r > L' '; });
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
    // LatexMath を Mermaid とキャッシュ衝突させないためのソルト（任意の定数）。
    // 描き方を変えたら値も変え、ディスクキャッシュに残った古い描き方の PNG を引かせない
    // (flowchart で包んでいた頃は 0xA1B2C3D4E5F60718)。
    constexpr uint64_t LATEX_MATH_HASH_SALT = 0x5C8E1F3A7B2D9064ULL;
    uint64_t h = Fnv1a64(node.GetText());
    if (node.code_language == SyntaxLanguage::LatexMath) {
        h ^= LATEX_MATH_HASH_SALT;
    }
    return h;
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
