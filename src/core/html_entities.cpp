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

    // "&#" / "&#x" と末尾 ';' を除いた数字部分。
    const bool hex = (entity[2] == 'x' || entity[2] == 'X');
    const std::string_view digits = entity.substr(hex ? 3 : 2, entity.size() - (hex ? 4 : 3));
    uint32_t codepoint = 0;
    const auto [stop, ec] = std::from_chars(digits.data(), digits.data() + digits.size(), codepoint, hex ? 16 : 10);
    // "&#65x;" のように途中で停止した入力は参照ではないので原文のまま残す。
    if (stop != digits.data() + digits.size() || (ec != std::errc{} && ec != std::errc::result_out_of_range)) {
        return entity;
    }
    // 文字にならない数 (0・サロゲート・U+10FFFF 超。uint32_t に収まらない数を wrap させない) は、CommonMark と GitHub と
    // 同じく U+FFFD にする。範囲外/サロゲート判定は EncodeCp 内に集約 (戻り値 0 で不正)。
    const uint32_t len = ec == std::errc{} && codepoint != 0 ? utf8_codec::EncodeCp(codepoint, buffer) : 0;
    return len > 0 ? std::string_view{ buffer, len } : utf8_codec::kReplacementUtf8;
}
