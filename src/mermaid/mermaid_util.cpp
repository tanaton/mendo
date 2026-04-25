#include "mermaid_util.h"
#include "document_types.h"
#include "utility.h"
#include <algorithm>
#include <cmath>
#include <format>
#include <iterator>
#include <limits>
#include <ranges>

std::pmr::wstring mermaid_util::JsEscape(std::wstring_view input)
{
    std::pmr::wstring result;
    result.reserve(input.size() + input.size() / 4);
    for (wchar_t c : input) {
        switch (c) {
        case L'\\': result += L"\\\\"; break;
        case L'\'': result += L"\\'"; break;
        case L'"':  result += L"\\\""; break;
        case L'\n': result += L"\\n"; break;
        case L'\r': result += L"\\r"; break;
        case L'\t': result += L"\\t"; break;
        case L'`':  result += L"\\`"; break;
        case L'$':  result += L"\\$"; break;
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

uint64_t mermaid_util::HashRaw(std::wstring_view input) noexcept
{
    return std::ranges::fold_left(input, 14695981039346656037ULL, [](uint64_t h, wchar_t c) static noexcept {
        return (h ^ static_cast<uint64_t>(c)) * 1099511628211ULL;
    });
}

uint64_t mermaid_util::HashRaw(std::string_view input) noexcept
{
    return std::ranges::fold_left(input, 14695981039346656037ULL, [](uint64_t h, char c) static noexcept {
        return (h ^ static_cast<uint64_t>(static_cast<unsigned char>(c))) * 1099511628211ULL;
    });
}

std::pmr::wstring mermaid_util::SimpleHash(std::wstring_view input)
{
    return PmrFormat(L"{:016x}", HashRaw(input));
}

uint64_t mermaid_util::CombinedHash(std::wstring_view code, int max_width_int, bool dark_mode) noexcept
{
    // コード全体をコピーせず、直接ハッシュして幅・モードをミックスする
    uint64_t h = HashRaw(code);
    h ^= static_cast<uint64_t>(max_width_int) * 1099511628211ULL;
    h ^= static_cast<uint64_t>(dark_mode) * 2654435761ULL;
    return h;
}

uint64_t mermaid_util::CombinedHash(std::string_view code, int max_width_int, bool dark_mode) noexcept
{
    uint64_t h = HashRaw(code);
    h ^= static_cast<uint64_t>(max_width_int) * 1099511628211ULL;
    h ^= static_cast<uint64_t>(dark_mode) * 2654435761ULL;
    return h;
}

int mermaid_util::ComputeWorkerCount(unsigned int processor_count) noexcept
{
    return std::clamp(static_cast<int>(processor_count) / 2, 2, 4);
}

int mermaid_util::QuantizeWidth(float max_width) noexcept
{
    if (!(max_width > 0.0f)) {
        return 100;
    }
    return static_cast<int>(std::ceil(max_width / 100.0f)) * 100;
}

uint64_t mermaid_util::HashCode(std::wstring_view code, float max_width, bool dark_mode) noexcept
{
    return CombinedHash(code, QuantizeWidth(max_width), dark_mode);
}

uint64_t mermaid_util::HashCode(std::string_view code, float max_width, bool dark_mode) noexcept
{
    return CombinedHash(code, QuantizeWidth(max_width), dark_mode);
}

std::pmr::wstring mermaid_util::BuildLatexFlowchartCode(std::wstring_view latex)
{
    // mermaid ラベル内で特殊文字をエスケープする:
    //   "  → #quot;  (ラベル終端の " と衝突)
    //   ]  → #93;    (ラベル終端の ] と衝突)
    //   \r, \n → 空白 (ラベル構文上改行不可)
    std::pmr::wstring result;
    result.reserve(latex.size() + 80);
    result.append(L"flowchart LR\n    A[\"$$");
    for (const wchar_t c : latex) {
        switch (c) {
        case L'"':  result.append(L"#quot;"); break;
        case L']':  result.append(L"#93;"); break;
        case L'\r': // fallthrough
        case L'\n':
            result.push_back(L' ');
            break;
        default:
            result.push_back(c);
            break;
        }
    }
    result.append(L"$$\"]\n    style A fill:none,stroke:none");
    return result;
}

float mermaid_util::ParseJsonNumber(std::wstring_view json, std::wstring_view key) noexcept
{
    auto pos = json.find(key);
    if (pos == std::wstring_view::npos) {
        return 0.0f;
    }
    pos += key.size();
    while (pos < json.size() && (json[pos] == L':' || json[pos] == L' ')) {
        pos++;
    }
    if (pos >= json.size()) {
        return 0.0f;
    }
    // wstring_view は null 終端が保証されないため、数値部分を切り出してから wcstof に渡す。
    wchar_t buf[64];
    const auto num_len = (std::min)(json.size() - pos, std::size(buf) - 1);
    std::char_traits<wchar_t>::copy(buf, json.data() + pos, num_len);
    buf[num_len] = L'\0';
    return std::wcstof(buf, nullptr);
}

mermaid_util::RequestPrefix mermaid_util::ParseRequestPrefix(std::wstring_view body) noexcept
{
    RequestPrefix out;
    if (body.empty() || body[0] < L'0' || body[0] > L'9') {
        return out;
    }
    // wstring_view から直接桁を読み取る。unsigned int に収まらない ID は無効扱いにする。
    constexpr uint64_t kMaxUint = std::numeric_limits<unsigned int>::max();
    uint64_t acc = 0;
    size_t i = 0;
    for (; i < body.size(); ++i) {
        const wchar_t c = body[i];
        if (c < L'0' || c > L'9') {
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

bool mermaid_util::ParseJsonTrueFlag(std::wstring_view json, std::wstring_view key) noexcept
{
    // 生成側が "ok":true / "ok": true の両形式を出す可能性があるため両方許容。
    auto pos = json.find(key);
    if (pos == std::wstring_view::npos) {
        return false;
    }
    pos += key.size();
    if (pos >= json.size() || json[pos] != L':') {
        return false;
    }
    ++pos;
    if (pos < json.size() && json[pos] == L' ') {
        ++pos;
    }
    constexpr std::wstring_view kTrue = L"true";
    if (json.size() - pos < kTrue.size()) {
        return false;
    }
    return json.compare(pos, kTrue.size(), kTrue) == 0;
}

uint64_t mermaid_util::NodeDiagramHash(const Node& node, float max_width, bool dark_mode) noexcept
{
    // LatexMath を Mermaid とキャッシュ衝突させないためのソルト（任意の定数）。
    constexpr uint64_t LATEX_MATH_HASH_SALT = 0xA1B2C3D4E5F60718ULL;
    uint64_t h = HashCode(node.GetText(), max_width, dark_mode);
    if (node.code_language == SyntaxLanguage::LatexMath) {
        h ^= LATEX_MATH_HASH_SALT;
    }
    return h;
}
