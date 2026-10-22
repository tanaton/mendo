#include "parser.h"
#include "html_entity_table.h"
#include "ascii_util.h"
#include "utf8_codec.h"
#include <algorithm>

namespace {

constexpr std::string_view EntityName(const HtmlNamedEntity& e) noexcept
{
    return { kHtmlEntityData + e.offset, e.name_len };
}

static_assert(std::ranges::is_sorted(kHtmlNamedEntities, {}, EntityName));

} // namespace

std::optional<std::string_view> ResolveHtmlEntity(std::string_view entity, char (&buffer)[4])
{
    if (entity.size() < 3 || entity[0] != '&' || entity.back() != ';') {
        return std::nullopt;
    }

    if (entity[1] != '#') {
        const std::string_view name = entity.substr(1, entity.size() - 2);
        const auto it = std::ranges::lower_bound(kHtmlNamedEntities, name, {}, EntityName);
        if (it == std::ranges::end(kHtmlNamedEntities) || EntityName(*it) != name) {
            return std::nullopt;
        }
        return std::string_view{ kHtmlEntityData + it->offset + it->name_len, it->utf8_len };
    }

    if (entity.size() < 4) {
        return std::nullopt;
    }

    // "&#" / "&#x" と末尾 ';' を除いた数字部分。最大桁数 (U+10FFFF = hex 6 桁 / 10 進 7 桁) を
    // 超える入力は codepoint (uint32_t) のラップを未然に防ぐため弾く。
    const bool hex = (entity[2] == 'x' || entity[2] == 'X');
    const std::string_view digits = entity.substr(hex ? 3 : 2, entity.size() - (hex ? 4 : 3));
    const size_t max_digits = hex ? 6 : 7;
    if (digits.empty() || digits.size() > max_digits) {
        return std::nullopt;
    }
    uint32_t codepoint = 0;
    const char* const stop = ascii_util::from_chars(digits.data(), digits.size(), codepoint, hex ? uint32_t{ 16 } : uint32_t{ 10 });
    // "&#65x;" のように途中で停止した入力は不正として弾く。
    if (stop != digits.data() + digits.size() || codepoint == 0) {
        return std::nullopt;
    }
    // 範囲外/サロゲート判定は EncodeCp 内に集約 (戻り値 0 で不正)。
    const uint32_t len = utf8_codec::EncodeCp(codepoint, buffer);
    if (len == 0) {
        return std::nullopt;
    }
    return std::string_view{ buffer, len };
}
