#pragma once
#include "config_store.h"
#include <string>

class ConfigService {
public:
    // 本番用: config_store の名前空間関数に委譲
    ConfigService() = default;

    void SaveBool(const wchar_t* key, bool value) { config::SaveBool(key, value); }
    bool LoadBool(const wchar_t* key, bool default_value = false) const { return config::LoadBool(key, default_value); }

    void SaveInt(const wchar_t* key, int value) { config::SaveInt(key, value); }
    int LoadInt(const wchar_t* key, int def, int min_v, int max_v) const { return config::LoadInt(key, def, min_v, max_v); }

    void SaveWString(const wchar_t* key, const std::wstring& value) { config::SaveWString(key, value); }
    std::wstring LoadWString(const wchar_t* key) const { return config::LoadWString(key); }
};
