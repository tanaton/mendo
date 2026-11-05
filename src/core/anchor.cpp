#include "anchor.h"
#include "ascii_util.h"
#include "unicode_lower_table.h"
#include "utf8_codec.h"
#include <algorithm>
#include <charconv>
#include <format>
#include <iterator>
#include <optional>
#include <system_error>

namespace {

constexpr std::string_view kFootnoteDefAnchorPrefix = "user-content-fn-";
constexpr std::string_view kFootnoteRefAnchorPrefix = "user-content-fnref-";

// s 全体が 1 以上の 10 進数なら値を返す。
std::optional<uint32_t> ParsePositive(std::string_view s) noexcept
{
    uint32_t value = 0;
    const auto [ptr, ec] = std::from_chars(s.data(), s.data() + s.size(), value);
    if (ec != std::errc{} || ptr != s.data() + s.size() || value == 0) {
        return std::nullopt;
    }
    return value;
}

// GitHub のスラグが使う toLowerCase に合わせる (UTF-8 長が伸びる変換は表から除いてある)。
constexpr uint32_t ToLowerNonAscii(uint32_t cp) noexcept
{
    // 見出しに多い CJK などは大文字を持たないので、表を引かずに返す (生成時に確認済み)。
    if (cp >= 0x3000 && cp < 0xA640) {
        return cp;
    }
    const auto it = std::ranges::lower_bound(kUnicodeLowerMappings, cp, {}, &UnicodeLowerMapping::upper);
    return it != std::ranges::end(kUnicodeLowerMappings) && it->upper == cp ? it->lower : cp;
}

static_assert(std::ranges::is_sorted(kUnicodeLowerMappings, {}, &UnicodeLowerMapping::upper));

// 非 ASCII の 1 コードポイントを小文字にして dst に書き、書いたバイト数を返す (decoded.len 以下)。
size_t WriteLowerNonAscii(utf8_codec::DecodedCp decoded, const char* src, char* dst) noexcept
{
    const uint32_t lower = ToLowerNonAscii(decoded.cp);
    if (lower == decoded.cp) {
        std::copy_n(src, decoded.len, dst);
        return decoded.len;
    }
    return utf8_codec::EncodeCp(lower, dst);
}

// アンカー (GitHub 互換スラグ) に採用しない記号・句読点か。非 ASCII コードポイント単位で判定。
// Unicode 全カテゴリテーブルは持たないため、主要な記号ブロックの近似で判定する
// (文字 (Letter) を誤って落とすより、稀な記号を通す方向に倒す)。
constexpr bool IsAnchorSkippableSymbol(uint32_t cp) noexcept
{
    // ラテン1補助の記号 (U+00A0–U+00BF)
    if (cp >= 0x00A0 && cp <= 0x00BF) {
        return true;
    }
    // 一般句読点・通貨・矢印・数学記号等 (U+2000–U+2BFF)、補助句読点 (U+2E00–U+2E7F)
    if ((cp >= 0x2000 && cp <= 0x2BFF) || (cp >= 0x2E00 && cp <= 0x2E7F)) {
        return true;
    }
    // CJK 記号と句読点 (U+3000–U+303F)
    if (cp >= 0x3000 && cp <= 0x303F) {
        return true;
    }
    // 全角 ASCII 対応の記号関連
    return (cp >= 0xFF01 && cp <= 0xFF0F) || (cp >= 0xFF1A && cp <= 0xFF20) || (cp >= 0xFF3B && cp <= 0xFF40) || (cp >= 0xFF5B && cp <= 0xFF65);
}

// kSlug なら GitHub のスラグ、そうでなければ小文字にするだけ。
// 出力は入力サイズ以下 (UTF-8 長が伸びる変換は表から除いてある) なので一括確保して書き出す。
template <bool kSlug>
std::string LowerAnchor(std::string_view text)
{
    std::string out;
    out.resize_and_overwrite(text.size(), [text](char* buf, size_t /*count*/) noexcept -> size_t {
        char* dst = buf;
        size_t pos = 0;
        while (pos < text.size()) {
            const char c = text[pos];
            if (static_cast<unsigned char>(c) < 0x80) {
                if (!kSlug || ascii_util::IsAsciiWordChar(c) || c == '-') {
                    *dst++ = ascii_util::ToLowerAscii(c);
                }
                else if (c == ' ' || c == '\t') {
                    *dst++ = '-';
                }
                ++pos;
                continue;
            }
            const auto decoded = utf8_codec::DecodeAt(text, static_cast<uint32_t>(pos));
            // U+FFFD は不正/truncated バイト由来。スラグには採用せず、小文字化では元のバイトを残す
            // (U+FFFD は小文字を持たないので WriteLowerNonAscii が写す。どの見出しとも一致しないままにする)。
            if (!kSlug || (decoded.cp != utf8_codec::kReplacement && !IsAnchorSkippableSymbol(decoded.cp))) {
                dst += WriteLowerNonAscii(decoded, text.data() + pos, dst);
            }
            pos += decoded.len;
        }
        return static_cast<size_t>(dst - buf);
    });
    return out;
}

} // namespace

std::string GenerateAnchorId(std::string_view text)
{
    return LowerAnchor<true>(text);
}

std::string ToLowerAnchorCopy(std::string_view text)
{
    return LowerAnchor<false>(text);
}

std::string_view HeadingAnchorIndex::Add(std::string_view heading_text, int node_index)
{
    std::string base = GenerateAnchorId(heading_text);
    // 挿入しなければ base はムーブされない。
    auto result = entries_.try_emplace(std::move(base), Entry{ node_index });
    if (!result.second) {
        // unordered_map は再ハッシュでも要素の参照が変わらない。
        int& counter = result.first->second.counter;
        do {
            result = entries_.try_emplace(std::format("{}-{}", base, ++counter), Entry{ node_index });
        } while (!result.second);
    }
    return result.first->first;
}

int HeadingAnchorIndex::Find(std::string_view anchor) const
{
    const auto it = entries_.find(anchor);
    return it != entries_.end() ? it->second.node_index : -1;
}

std::string FootnoteDefUrl(uint32_t id)
{
    return std::format("#{}{}", kFootnoteDefAnchorPrefix, id);
}

std::string FootnoteRefUrl(uint32_t id, uint32_t ref_id)
{
    auto url = std::format("#{}{}", kFootnoteRefAnchorPrefix, id);
    // GitHub と同じく 1 回目の参照は番号を付けない。
    if (ref_id > 1) {
        std::format_to(std::back_inserter(url), "-{}", ref_id);
    }
    return url;
}

int FindFootnoteAnchor(std::string_view anchor, std::span<const int32_t> def_nodes, std::span<const FootnoteRef> refs) noexcept
{
    // "fnref-" は "fn-" で始まらないので判定順は問わない。
    if (anchor.starts_with(kFootnoteDefAnchorPrefix)) {
        const auto id = ParsePositive(anchor.substr(kFootnoteDefAnchorPrefix.size()));
        return (id && *id <= def_nodes.size()) ? def_nodes[*id - 1] : -1;
    }
    if (!anchor.starts_with(kFootnoteRefAnchorPrefix)) {
        return -1;
    }
    const auto rest = anchor.substr(kFootnoteRefAnchorPrefix.size());
    const auto dash = rest.find('-');
    const auto id = ParsePositive(rest.substr(0, dash));
    const auto ref_id = dash == std::string_view::npos ? std::optional<uint32_t>{ 1 } : ParsePositive(rest.substr(dash + 1));
    if (!id || !ref_id) {
        return -1;
    }
    const auto it = std::ranges::find_if(refs, [&](const FootnoteRef& r) noexcept {
        return r.id == *id && r.ref_id == *ref_id;
    });
    return it != refs.end() ? it->node_index : -1;
}
