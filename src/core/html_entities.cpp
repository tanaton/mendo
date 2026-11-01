#include "parser.h"
#include "html_entity_table.h"
#include "utf8_codec.h"
#include <algorithm>
#include <charconv>

namespace {

constexpr std::string_view EntityName(const HtmlNamedEntity& e) noexcept
{
    return { kHtmlEntityData + e.offset, e.name_len };
}

static_assert(std::ranges::is_sorted(kHtmlNamedEntities, {}, EntityName));

} // namespace

std::string_view ResolveHtmlEntity(std::string_view entity, char (&buffer)[4])
{
    if (entity.size() < 3 || entity[0] != '&' || entity.back() != ';') {
        return entity;
    }

    if (entity[1] != '#') {
        const std::string_view name = entity.substr(1, entity.size() - 2);
        const auto it = std::ranges::lower_bound(kHtmlNamedEntities, name, {}, EntityName);
        if (it == std::ranges::end(kHtmlNamedEntities) || EntityName(*it) != name) {
            return entity;
        }
        return { kHtmlEntityData + it->offset + it->name_len, it->utf8_len };
    }

    // "&#" / "&#x" と末尾 ';' を除いた数字部分。uint32_t に収まらない数は from_chars が out_of_range で弾く。
    const bool hex = (entity[2] == 'x' || entity[2] == 'X');
    const std::string_view digits = entity.substr(hex ? 3 : 2, entity.size() - (hex ? 4 : 3));
    uint32_t codepoint = 0;
    const auto [stop, ec] = std::from_chars(digits.data(), digits.data() + digits.size(), codepoint, hex ? 16 : 10);
    // "&#65x;" のように途中で停止した入力は不正として弾く。
    if (ec != std::errc{} || stop != digits.data() + digits.size() || codepoint == 0) {
        return entity;
    }
    // 範囲外/サロゲート判定は EncodeCp 内に集約 (戻り値 0 で不正)。
    const uint32_t len = utf8_codec::EncodeCp(codepoint, buffer);
    return len > 0 ? std::string_view{ buffer, len } : entity;
}
