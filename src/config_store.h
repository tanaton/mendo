#pragma once
#include <string>
#include <filesystem>

namespace config {

// Override config directory (for testing). Pass empty path to reset to default.
void SetConfigDirOverride(const std::filesystem::path& dir);

// Returns the mendo config directory (%LOCALAPPDATA%/mendo).
std::filesystem::path GetConfigDir();

// Returns full path for a config file within the config directory.
std::filesystem::path GetConfigPath(const wchar_t* filename);

// Save/load a boolean value (stored as '0' or '1').
void SaveBool(const wchar_t* filename, bool value);
bool LoadBool(const wchar_t* filename, bool default_value = false);

// Save/load an integer value with bounds checking.
void SaveInt(const wchar_t* filename, int value);
int LoadInt(const wchar_t* filename, int default_value, int min_val, int max_val);

// Save/load a wide string (stored as UTF-16LE binary).
void SaveWString(const wchar_t* filename, std::wstring_view value);
std::wstring LoadWString(const wchar_t* filename);

} // namespace config
