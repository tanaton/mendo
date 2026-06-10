#pragma once
#include <optional>
#include <string_view>

// HTML エンティティ (例: "&amp;", "&#x1F600;") を解決する。
// view が指す領域は (a) static なリテラル または (b) 呼び出し側が渡した buffer のいずれか。
// buffer のスコープ内でのみ valid。UTF-8 では U+10FFFF までを最大 4 byte で表現できる。
[[nodiscard]] std::optional<std::string_view> ResolveHtmlEntity(std::string_view entity, char (&buffer)[4]);
