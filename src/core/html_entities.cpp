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

    if (entity.size() >= 4 && entity[0] == '&' && entity[1] == '#' && entity.back() == ';') {
        const char* digits;
        size_t digit_len;
        uint32_t base;
        size_t max_digits;
        if (entity[2] == 'x' || entity[2] == 'X') {
            digits = entity.data() + 3;
            digit_len = entity.size() - 4; // "&#x" と末尾 ';' を除いた残り長
            base = 16;
            max_digits = 6; // U+10FFFF = 6 桁。これより長い hex 入力は overflow の前に弾く。
        }
        else {
            digits = entity.data() + 2;
            digit_len = entity.size() - 3; // "&#" と末尾 ';' を除いた残り長
            base = 10;
            max_digits = 7; // 1114111 = 7 桁。これより長い 10 進入力は overflow の前に弾く。
        }
        // 桁数オーバーは codepoint 型 (uint32_t) のラップを未然に防ぐため弾く。
        if (digit_len == 0 || digit_len > max_digits) {
            return std::nullopt;
        }
        uint32_t codepoint = 0;
        const char* const stop = ascii_util::from_chars(digits, digit_len, codepoint, base);
        // 全桁消費 (stop == digits + digit_len) のみ受理。
        // "&#65x;" のように途中で停止した入力は不正として弾く。
        if (stop != digits + digit_len || codepoint == 0) {
            return std::nullopt;
        }
        // 範囲外/サロゲート判定は EncodeCp 内に集約 (戻り値 0 で不正)。
        const uint32_t len = utf8_codec::EncodeCp(codepoint, buffer);
        if (len == 0) {
            return std::nullopt;
        }
        return std::string_view{ buffer, len };
    }

    return std::nullopt;
}
