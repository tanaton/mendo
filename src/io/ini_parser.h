#pragma once
#include <map>
#include <string>
#include <string_view>

namespace ini {

// セクション名 → (キー名 → 値) のマッピング
using IniData = std::map<std::string, std::map<std::string, std::string, std::less<>>, std::less<>>;

// INIテキストをパースして構造化データに変換する。
// - [Section] でセクション開始
// - Key=Value でキー値ペア（最初の '=' で分割）
// - ';' または '#' で始まる行はコメント（無視）
// - 空行は無視
// - セクション外のキーは空文字列セクションに格納
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
        std::string_view line = text.substr(pos, eol - pos);

        // \r\n を 1 行として進める（CRLF/CR/LF を統一して扱う）。
        pos = eol;
        if (pos < text.size() && text[pos] == '\r') {
            ++pos;
        }
        if (pos < text.size() && text[pos] == '\n') {
            ++pos;
        }

        const size_t start = line.find_first_not_of(" \t");
        if (start == std::string_view::npos) {
            continue;
        }
        line = line.substr(start);

        if (line[0] == ';' || line[0] == '#') {
            continue;
        }

        if (line[0] == '[') {
            const size_t close = line.find(']', 1);
            if (close != std::string_view::npos) {
                current_section = std::string(line.substr(1, close - 1));
            }
            continue;
        }

        const size_t eq = line.find('=');
        if (eq == std::string_view::npos) {
            continue;
        }

        const std::string_view key_part = line.substr(0, eq);
        const size_t key_end = key_part.find_last_not_of(" \t");
        std::string key;
        if (key_end != std::string_view::npos) {
            key = std::string(key_part.substr(0, key_end + 1));
        }
        if (key.empty()) {
            continue;
        }

        const std::string_view val_part = line.substr(eq + 1);
        const size_t val_start = val_part.find_first_not_of(" \t");
        std::string value;
        if (val_start != std::string_view::npos) {
            const size_t val_end = val_part.find_last_not_of(" \t");
            value = std::string(val_part.substr(val_start, val_end - val_start + 1));
        }

        data[current_section][std::move(key)] = std::move(value);
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
