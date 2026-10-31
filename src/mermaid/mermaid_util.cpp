#include "mermaid_util.h"
#include "document_types.h"
#include "utility.h"
#include "syntax.h"
#include "ascii_util.h"
#include "string_convert.h"
#include <algorithm>
#include <bit>
#include <cmath>
#include <cwchar>
#include <format>
#include <iterator>
#include <limits>

using namespace std::literals;

std::wstring mermaid_util::PageRenderMessage(PageRequest kind, unsigned id, bool dark, std::string_view code_utf8)
{
    static constexpr std::wstring_view kKinds[]{ L"mermaid", L"svg", L"view" };
    std::wstring msg = std::format(L"{}:{}:{}:", kKinds[std::to_underlying(kind)], id, dark ? 1 : 0);
    msg += string_convert::Utf8ToWide(code_utf8);
    return msg;
}

std::wstring mermaid_util::PageMathBatchMessage(unsigned id, int page_width)
{
    return std::format(L"math:{}:{}:", id, page_width);
}

void mermaid_util::AppendPageMathItem(std::wstring& msg, std::string_view tex_utf8, float font_size)
{
    const std::wstring tex = string_convert::Utf8ToWide(tex_utf8);
    std::format_to(std::back_inserter(msg), L"{:.3f}:{}:", font_size, tex.size());
    msg += tex;
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

std::wstring mermaid_util::ViewerLoadingMessage(bool dark)
{
    return std::format(L"loading:{}:", dark ? 1 : 0);
}

std::wstring mermaid_util::ViewerSvgMessage(bool dark, std::wstring svg)
{
    svg.insert(0, std::format(L"svg:{}:", dark ? 1 : 0));
    return svg;
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
constexpr uint8_t kIendChunkType[4]{ 'I', 'E', 'N', 'D' };

// mdMB のデータ。版・ページの幅・高さ・DPR・箱の数と、箱ごとに x・y・幅・高さ・ベースライン (どれも 4 バイト)。
constexpr size_t MathBoxesDataSize(size_t count) noexcept
{
    return 20 + count * 20;
}

uint32_t Crc32(std::span<const uint8_t> bytes) noexcept
{
    uint32_t crc = 0xFFFFFFFFu;
    for (const uint8_t byte : bytes) {
        crc ^= byte;
        for (int k = 0; k < 8; ++k) {
            crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
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

ByteBuffer mermaid_util::AppendMathBoxesChunk(std::span<const uint8_t> png, const MathBatchLayout& layout)
{
    // IEND (長さ 0 + 種類 + CRC の 12 バイト) の直前に差し込む。
    constexpr size_t kIendSize = 12;
    if (png.size() < sizeof(kPngSignature) + kIendSize || !std::ranges::starts_with(png, kPngSignature) ||
        !std::ranges::equal(png.last(kIendSize).subspan(4, 4), kIendChunkType)) {
        return {};
    }
    // 長さ (4) + 種類 (4) + データ + CRC (4)。CRC は種類とデータに掛ける。
    const auto data_size = static_cast<uint32_t>(MathBoxesDataSize(layout.boxes.size()));
    std::vector<uint8_t> chunk;
    chunk.reserve(data_size + 12);
    PutBe32(chunk, data_size);
    chunk.insert(chunk.end(), std::begin(kMathBoxesChunkType), std::end(kMathBoxesChunkType));
    PutBe32(chunk, kMathBoxesChunkVersion);
    const auto put_float = [&chunk](float v) { PutBe32(chunk, std::bit_cast<uint32_t>(v)); };
    put_float(layout.size.width);
    put_float(layout.size.height);
    put_float(layout.size.dpr);
    PutBe32(chunk, static_cast<uint32_t>(layout.boxes.size()));
    for (const auto& b : layout.boxes) {
        for (const float v : { b.x, b.y, b.width, b.height, b.baseline }) {
            put_float(v);
        }
    }
    PutBe32(chunk, Crc32(std::span{ chunk }.subspan(4)));
    ByteBuffer out(png.size() + chunk.size());
    auto* p = std::ranges::copy(png.first(png.size() - kIendSize), out.data()).out;
    p = std::ranges::copy(chunk, p).out;
    std::ranges::copy(png.last(kIendSize), p);
    return out;
}

std::optional<mermaid_util::MathBatchLayout> mermaid_util::ReadMathBoxesChunk(std::span<const uint8_t> png, size_t expected)
{
    if (!std::ranges::starts_with(png, kPngSignature)) {
        return std::nullopt;
    }
    for (size_t pos = sizeof(kPngSignature); pos + 12 <= png.size();) {
        const uint32_t length = GetBe32(&png[pos]);
        if (length > png.size() - pos - 12) {
            return std::nullopt;
        }
        if (!std::ranges::equal(png.subspan(pos + 4, 4), kMathBoxesChunkType)) {
            pos += size_t{ 12 } + length;
            continue;
        }
        const auto data = png.subspan(pos + 8, length);
        if (GetBe32(&png[pos + 8 + length]) != Crc32(png.subspan(pos + 4, 4 + length)) || data.size() != MathBoxesDataSize(expected)) {
            return std::nullopt;
        }
        const auto get_float = [&data](size_t i) { return std::bit_cast<float>(GetBe32(&data[i * 4])); };
        if (GetBe32(&data[0]) != kMathBoxesChunkVersion || GetBe32(&data[16]) != expected) {
            return std::nullopt;
        }
        MathBatchLayout out{
            .size = { get_float(1), get_float(2), get_float(3) }
        };
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
    // "<w>:<h>:<dpr>|" の後に箱が並ぶ。ページの大きさが正でなければ読まない。
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
            float v[5]{};
            ParseFloatList(item, L',', v);
            box = { v[0], v[1], v[2], v[3], v[4] };
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
