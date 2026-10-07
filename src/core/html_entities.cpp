#include "html_entities.h"
#include "ascii_util.h"
#include "utf8_codec.h"

std::optional<std::string_view> ResolveHtmlEntity(std::string_view entity, char (&buffer)[4])
{
    // 名前付き実体参照はサイズで先に分岐し、比較対象を 1～3 候補に絞る。
    switch (entity.size()) {
    case 4:
        if (entity == "&lt;") {
            return std::string_view{ "<" };
        }
        if (entity == "&gt;") {
            return std::string_view{ ">" };
        }
        break;
    case 5:
        if (entity == "&amp;") {
            return std::string_view{ "&" };
        }
        break;
    case 6:
        if (entity == "&quot;") {
            return std::string_view{ "\"" };
        }
        if (entity == "&apos;") {
            return std::string_view{ "'" };
        }
        if (entity == "&nbsp;") {
            return std::string_view{ "\xC2\xA0" }; // U+00A0 NO-BREAK SPACE (UTF-8: C2 A0)
        }
        break;
    default:
        break;
    }

    if (entity.size() < 4 || entity[0] != '&' || entity[1] != '#' || entity.back() != ';') {
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
