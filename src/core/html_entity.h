#pragma once
#include <optional>
#include <string_view>

// HTML エンティティ (例: "&amp;", "&#x1F600;") を解決する。
// 戻り値: 解決成功なら wide 文字列の view、失敗なら nullopt (呼び出し側で元の utf-8 を
// そのままテキストとして再投入することを示す)。
// view が指す領域は (a) static なリテラル または (b) 呼び出し側が渡した buffer のいずれか。
// buffer のスコープ内でのみ valid。
[[nodiscard]] std::optional<std::wstring_view> ResolveHtmlEntity(std::string_view entity, wchar_t (&buffer)[2]);
