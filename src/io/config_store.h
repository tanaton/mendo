#pragma once
#include <string>
#include <string_view>
#include <filesystem>
#include <memory_resource>

namespace config {

void SetConfigDirOverride(const std::filesystem::path& dir);
std::filesystem::path GetConfigDir();
std::filesystem::path GetConfigPath(std::wstring_view filename);
void Load();
void Save();
void Clear() noexcept;

// ---- 型付きアクセサ（メモリ上のマップを読み書き） ----

void SetBool(std::string_view section, std::string_view key, bool value);
bool GetBool(std::string_view section, std::string_view key, bool default_value = false);

void SetInt(std::string_view section, std::string_view key, int value);
int GetInt(std::string_view section, std::string_view key, int default_value, int min_val, int max_val);

void SetWString(std::string_view section, std::string_view key, std::wstring_view value);
std::pmr::wstring GetWString(std::string_view section, std::string_view key);

} // namespace config
