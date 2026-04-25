#include "config_service.h"
#include "string_convert.h"
#include "file_io.h"
#include <algorithm>
#include <charconv>
#include <ranges>
#include <shlobj.h>

#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "ole32.lib")

// ============================================================
// ディレクトリ・パスヘルパー
// ============================================================

void ConfigService::SetConfigDirOverride(const std::filesystem::path& dir)
{
    config_dir_override_ = dir;
}

std::filesystem::path ConfigService::GetConfigDir() const
{
    if (!config_dir_override_.empty()) {
        return config_dir_override_;
    }
    if (!cached_default_dir_.empty()) {
        return cached_default_dir_;
    }
    wchar_t* appdata = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &appdata))) {
        return {};
    }
    cached_default_dir_ = std::filesystem::path(appdata) / L"mendo";
    CoTaskMemFree(appdata);
    return cached_default_dir_;
}

std::filesystem::path ConfigService::GetConfigPath(std::wstring_view filename) const
{
    if (filename.empty()) {
        return {};
    }
    if (std::ranges::any_of(filename, [](wchar_t c) static noexcept {
        return c == L'\\' || c == L'/' || c == L':';
    })) {
        return {};
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
// Load / Flush / Clear
// ============================================================

void ConfigService::Load()
{
    const auto dir = GetConfigDir();
    if (dir.empty()) {
        return;
    }

    const auto ini_path = dir / L"settings.ini";
    auto [buf, size] = ReadAllBytes(ini_path);
    if (!buf) {
        return;
    }
    data_ = ini::Parse(std::string_view(reinterpret_cast<const char*>(buf.get()), size));
}

void ConfigService::Flush()
{
    const auto dir = GetConfigDir();
    if (dir.empty()) {
        return;
    }
    std::filesystem::create_directories(dir);

    const auto ini_path = dir / L"settings.ini";
    const auto tmp_path = dir / L"settings.ini.tmp";

    const std::string content = ini::Serialize(data_);
    if (!WriteAllBytes(tmp_path, content.data(), content.size())) {
        return;
    }

    if (!MoveFileExW(tmp_path.c_str(), ini_path.c_str(), MOVEFILE_REPLACE_EXISTING)) {
        // renameが失敗した場合（クロスボリューム等）、直接書き込み
        DeleteFileW(tmp_path.c_str());
        WriteAllBytes(ini_path, content.data(), content.size());
    }
}

void ConfigService::Clear() noexcept
{
    data_.clear();
}

// ============================================================
// 型付きアクセサ
// ============================================================

const std::string* ConfigService::FindValue(std::string_view section, std::string_view key) const
{
    const auto sit = data_.find(section);
    if (sit == data_.end()) {
        return nullptr;
    }
    const auto kit = sit->second.find(key);
    if (kit == sit->second.end()) {
        return nullptr;
    }
    return &kit->second;
}

void ConfigService::SaveBool(std::string_view section, std::string_view key, bool value)
{
    data_[std::string(section)][std::string(key)] = value ? "1" : "0";
}

bool ConfigService::LoadBool(std::string_view section, std::string_view key, bool default_value) const
{
    const auto* val = FindValue(section, key);
    return val ? (*val == "1") : default_value;
}

void ConfigService::SaveInt(std::string_view section, std::string_view key, int value)
{
    data_[std::string(section)][std::string(key)] = std::to_string(value);
}

int ConfigService::LoadInt(std::string_view section, std::string_view key, int def, int min_v, int max_v) const
{
    const auto* val = FindValue(section, key);
    if (!val) {
        return def;
    }
    int result = def;
    const auto [ptr, ec] = std::from_chars(val->data(), val->data() + val->size(), result);
    if (ec != std::errc{}) {
        return def;
    }
    if (result < min_v || result > max_v) {
        return def;
    }
    return result;
}

void ConfigService::SaveWString(std::string_view section, std::string_view key, std::wstring_view value)
{
    data_[std::string(section)][std::string(key)] = string_convert::WideToUtf8(value);
}

std::pmr::wstring ConfigService::LoadWString(std::string_view section, std::string_view key) const
{
    const auto* val = FindValue(section, key);
    return val ? string_convert::Utf8ToWide(*val) : std::pmr::wstring{};
}
