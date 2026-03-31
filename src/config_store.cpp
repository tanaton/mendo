#include "config_store.h"
#include "ini_parser.h"
#include "string_convert.h"
#include <charconv>
#include <fstream>
#include <shlobj.h>

#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "ole32.lib")

namespace config {

static std::filesystem::path g_config_dir_override;
static ini::IniData g_data;

// ============================================================
// ディレクトリ・パスヘルパー
// ============================================================

void SetConfigDirOverride(const std::filesystem::path& dir)
{
    g_config_dir_override = dir;
}

std::filesystem::path GetConfigDir()
{
    if (!g_config_dir_override.empty()) {
        return g_config_dir_override;
    }
    wchar_t* appdata = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &appdata))) {
        return {};
    }
    const std::filesystem::path dir = std::filesystem::path(appdata) / L"mendo";
    CoTaskMemFree(appdata);
    return dir;
}

std::filesystem::path GetConfigPath(std::wstring_view filename)
{
    if (filename.empty()) {
        return {};
    }
    for (wchar_t c : filename) {
        if (c == L'\\' || c == L'/' || c == L':') {
            return {};
        }
    }
    if (filename.find(L"..") != std::wstring_view::npos) {
        return {};
    }
    const auto dir = GetConfigDir();
    if (dir.empty()) {
        return {};
    }
    return dir / filename;
}

// ============================================================
// Load / Save / Clear
// ============================================================

void Load()
{
    const auto dir = GetConfigDir();
    if (dir.empty()) {
        return;
    }

    const auto ini_path = dir / L"settings.ini";
    std::ifstream ifs(ini_path, std::ios::binary);
    if (ifs) {
        const std::string content((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
        g_data = ini::Parse(content);
    }
}

void Save()
{
    const auto dir = GetConfigDir();
    if (dir.empty()) {
        return;
    }
    std::filesystem::create_directories(dir);

    const auto ini_path = dir / L"settings.ini";
    const auto tmp_path = dir / L"settings.ini.tmp";

    const std::string content = ini::Serialize(g_data);
    {
        std::ofstream ofs(tmp_path, std::ios::binary);
        if (!ofs) {
            return;
        }
        ofs.write(content.data(), static_cast<std::streamsize>(content.size()));
        if (!ofs) {
            return;
        }
    }

    std::error_code ec;
    std::filesystem::rename(tmp_path, ini_path, ec);
    if (ec) {
        // renameが失敗した場合（クロスボリューム等）、直接書き込み
        std::filesystem::remove(tmp_path, ec);
        std::ofstream ofs(ini_path, std::ios::binary);
        if (ofs) {
            ofs.write(content.data(), static_cast<std::streamsize>(content.size()));
        }
    }
}

void Clear()
{
    g_data.clear();
}

// ============================================================
// 型付きアクセサ
// ============================================================

static const std::string* FindValue(std::string_view section, std::string_view key)
{
    const auto sit = g_data.find(section);
    if (sit == g_data.end()) {
        return nullptr;
    }
    const auto kit = sit->second.find(key);
    if (kit == sit->second.end()) {
        return nullptr;
    }
    return &kit->second;
}

void SetBool(std::string_view section, std::string_view key, bool value)
{
    g_data[std::string(section)][std::string(key)] = value ? "1" : "0";
}

bool GetBool(std::string_view section, std::string_view key, bool default_value)
{
    const auto* val = FindValue(section, key);
    return val ? (*val == "1") : default_value;
}

void SetInt(std::string_view section, std::string_view key, int value)
{
    g_data[std::string(section)][std::string(key)] = std::to_string(value);
}

int GetInt(std::string_view section, std::string_view key, int default_value, int min_val, int max_val)
{
    const auto* val = FindValue(section, key);
    if (!val) {
        return default_value;
    }
    int result = default_value;
    const auto [ptr, ec] = std::from_chars(val->data(), val->data() + val->size(), result);
    if (ec != std::errc{}) {
        return default_value;
    }
    if (result < min_val || result > max_val) {
        return default_value;
    }
    return result;
}

void SetWString(std::string_view section, std::string_view key, std::wstring_view value)
{
    g_data[std::string(section)][std::string(key)] = string_convert::WideToUtf8(value);
}

std::pmr::wstring GetWString(std::string_view section, std::string_view key)
{
    const auto* val = FindValue(section, key);
    return val ? string_convert::Utf8ToWide(*val) : std::pmr::wstring{};
}

} // namespace config
