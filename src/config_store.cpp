#include "config_store.h"
#include <fstream>
#include <shlobj.h>

#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "ole32.lib")

namespace config {

static std::filesystem::path g_config_dir_override;

void SetConfigDirOverride(const std::filesystem::path& dir) {
    g_config_dir_override = dir;
}

std::filesystem::path GetConfigDir() {
    if (!g_config_dir_override.empty()) {
        return g_config_dir_override;
    }
    wchar_t* appdata = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &appdata))) {
        return {};
    }
    std::filesystem::path dir = std::filesystem::path(appdata) / L"mendo";
    CoTaskMemFree(appdata);
    return dir;
}

std::filesystem::path GetConfigPath(std::wstring_view filename) {
    auto dir = GetConfigDir();
    if (dir.empty()) return {};
    return dir / filename;
}

void SaveBool(std::wstring_view filename, bool value) {
    auto path = GetConfigPath(filename);
    if (path.empty()) return;
    std::filesystem::create_directories(path.parent_path());
    std::ofstream ofs(path);
    if (ofs) ofs << (value ? "1" : "0");
}

bool LoadBool(std::wstring_view filename, bool default_value) {
    auto path = GetConfigPath(filename);
    if (path.empty()) return default_value;
    std::ifstream ifs(path);
    if (!ifs) return default_value;
    char c = '0';
    ifs >> c;
    return c == '1';
}

void SaveInt(std::wstring_view filename, int value) {
    auto path = GetConfigPath(filename);
    if (path.empty()) return;
    std::filesystem::create_directories(path.parent_path());
    std::ofstream ofs(path);
    if (ofs) ofs << value;
}

int LoadInt(std::wstring_view filename, int default_value, int min_val, int max_val) {
    auto path = GetConfigPath(filename);
    if (path.empty()) return default_value;
    std::ifstream ifs(path);
    if (!ifs) return default_value;
    int val = default_value;
    if (!(ifs >> val)) return default_value;
    if (val < min_val || val > max_val) return default_value;
    return val;
}

void SaveWString(std::wstring_view filename, std::wstring_view value) {
    auto path = GetConfigPath(filename);
    if (path.empty()) return;
    if (value.empty()) {
        std::filesystem::remove(path);
        return;
    }
    std::filesystem::create_directories(path.parent_path());
    std::ofstream ofs(path, std::ios::binary);
    if (ofs) {
        ofs.write(reinterpret_cast<const char*>(value.data()),
                  static_cast<std::streamsize>(value.size() * sizeof(wchar_t)));
    }
}

std::pmr::wstring LoadWString(std::wstring_view filename) {
    auto path = GetConfigPath(filename);
    if (path.empty()) return {};
    std::ifstream ifs(path, std::ios::binary | std::ios::ate);
    if (!ifs) return {};
    auto size = ifs.tellg();
    if (size <= 0 || size % sizeof(wchar_t) != 0) return {};
    ifs.seekg(0);
    std::pmr::wstring result(static_cast<size_t>(size) / sizeof(wchar_t), L'\0');
    ifs.read(reinterpret_cast<char*>(result.data()), size);
    return result;
}

} // namespace config
