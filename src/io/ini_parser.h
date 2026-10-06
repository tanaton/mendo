#pragma once
#include <map>
#include <string>
#include <string_view>

namespace ini {

using IniData = std::map<std::string, std::map<std::string, std::string, std::less<>>, std::less<>>;

namespace detail {
constexpr std::string_view TrimBlanks(std::string_view s) noexcept
{
    const size_t first = s.find_first_not_of(" \t");
    if (first == std::string_view::npos) {
        return {};
    }
    return s.substr(first, s.find_last_not_of(" \t") - first + 1);
}
} // namespace detail

// セクション外のキーは空文字列セクションに格納。
inline IniData Parse(std::string_view text)
{
    IniData data;
    std::string current_section;

    size_t pos = 0;
    while (pos < text.size()) {
        size_t eol = text.find_first_of("\r\n", pos);
        if (eol == std::string_view::npos) {
            eol = text.size();
        }
        const std::string_view line = detail::TrimBlanks(text.substr(pos, eol - pos));

        pos = eol;
        if (pos < text.size() && text[pos] == '\r') {
            ++pos;
        }
        if (pos < text.size() && text[pos] == '\n') {
            ++pos;
        }

        if (line.empty() || line[0] == ';' || line[0] == '#') {
            continue;
        }

        if (line[0] == '[') {
            const size_t close = line.find(']', 1);
            if (close != std::string_view::npos) {
                current_section = std::string(line.substr(1, close - 1));
            }
            else {
                // 未終端ブラケットは壊れたセクション開始。後続キーが直前セクションへ
                // 誤混入するのを防ぐため無名セクションへ退避する。
                current_section.clear();
            }
            continue;
        }

        const size_t eq = line.find('=');
        if (eq == std::string_view::npos) {
            continue;
        }

        const std::string_view key = detail::TrimBlanks(line.substr(0, eq));
        if (key.empty()) {
            continue;
        }
        data[current_section][std::string(key)] = std::string(detail::TrimBlanks(line.substr(eq + 1)));
    }

    return data;
}

inline std::string Serialize(const IniData& data)
{
    std::string result;
    bool first_section = true;

    for (const auto& [section, kvs] : data) {
        if (kvs.empty()) {
            continue;
        }
        if (!first_section) {
            result += '\n';
        }
        first_section = false;

        if (!section.empty()) {
            result += '[';
            result += section;
            result += "]\n";
        }

        for (const auto& [key, value] : kvs) {
            result += key;
            result += '=';
            result += value;
            result += '\n';
        }
    }

    return result;
}

} // namespace ini
