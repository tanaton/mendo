#include "mermaid_util.h"
#include "types.h"
#include "utility.h"
#include <algorithm>
#include <cmath>
#include <format>
#include <iterator>
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

uint64_t mermaid_util::NodeDiagramHash(const Node& node, float max_width, bool dark_mode) noexcept
{
    // LatexMath を Mermaid とキャッシュ衝突させないためのソルト（任意の定数）。
    constexpr uint64_t LATEX_MATH_HASH_SALT = 0xA1B2C3D4E5F60718ULL;
    uint64_t h = HashCode(node.text_utf8, max_width, dark_mode);
    if (node.code_language == SyntaxLanguage::LatexMath) {
        h ^= LATEX_MATH_HASH_SALT;
    }
    return h;
}
