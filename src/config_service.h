#pragma once
#include "config_store.h"
#include <string>
#include <string_view>
#include <memory_resource>

class ConfigService {
public:
    ConfigService() = default;

    void SaveBool(std::string_view section, std::string_view key, bool value) { config::SetBool(section, key, value); }
    bool LoadBool(std::string_view section, std::string_view key, bool default_value = false) const { return config::GetBool(section, key, default_value); }

    void SaveInt(std::string_view section, std::string_view key, int value) { config::SetInt(section, key, value); }
    int LoadInt(std::string_view section, std::string_view key, int def, int min_v, int max_v) const { return config::GetInt(section, key, def, min_v, max_v); }

    void SaveWString(std::string_view section, std::string_view key, std::wstring_view value) { config::SetWString(section, key, value); }
    std::pmr::wstring LoadWString(std::string_view section, std::string_view key) const { return config::GetWString(section, key); }

    // メモリ上のデータをディスクに書き出す
    void Flush() { config::Save(); }
};
