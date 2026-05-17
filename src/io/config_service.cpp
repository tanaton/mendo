#include "config_service.h"
#include "string_convert.h"
#include "file_io.h"
#include <algorithm>
#include <charconv>
#include <cmath>
#include <ranges>
#include <shlobj.h>
#include <windows.h>

#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "ole32.lib")

void ConfigService::SetConfigDirOverride(const std::filesystem::path& dir)
{
    config_dir_override_ = dir;
}

const std::filesystem::path& ConfigService::DefaultConfigDir()
{
    // magic static: SHGetKnownFolderPath はプロセス全体で 1 回のみ呼び出される。
    // 失敗時は空 path のままキャッシュされ、callsite (GetConfigPath/Load/Flush) で空チェックされる。
    static const std::filesystem::path kDir = []() noexcept {
        wchar_t* appdata = nullptr;
        std::filesystem::path result;
        if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &appdata))) {
            result = std::filesystem::path(appdata) / L"mendo";
            CoTaskMemFree(appdata);
        }
        return result;
    }();
    return kDir;
}

std::filesystem::path ConfigService::GetConfigDir() const
{
    if (!config_dir_override_.empty()) {
        return config_dir_override_;
    }
    return DefaultConfigDir();
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
    // `..` の substring 判定だと `foo...bar.txt` も誤弾きするので equality で判定する。
    if (filename == L".." || filename == L".") {
        return {};
    }
    const auto dir = GetConfigDir();
    if (dir.empty()) {
        return {};
    }
    return dir / filename;
}

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
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    if (ec) {
        return;
    }

    const auto ini_path = dir / L"settings.ini";
    const std::string content = ini::Serialize(data_);
    (void)AtomicWriteAllBytes(ini_path, content.data(), content.size());
}

void ConfigService::Clear() noexcept
{
    data_.clear();
}

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

void SessionService::SaveLastFilePath(std::wstring_view path)
{
    if (path.empty()) {
        return;
    }
    config_.SaveWString("Session", "LastFile", path);
}

std::pmr::wstring SessionService::LoadLastFilePath() const
{
    std::pmr::wstring path = config_.LoadWString("Session", "LastFile");
    if (path.empty()) {
        return {};
    }
    // UNCパス (\\server\...) やデバイスパス (\\.\, \\?\) をブロックしてローカルパスのみ許可。
    if (path.size() >= 2 && path[0] == L'\\' && path[1] == L'\\') {
        return {};
    }
    if (GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES) {
        return {};
    }
    return path;
}

void SessionService::SavePaneState(const PaneState& state)
{
    config_.SaveBool("Pane", "ShowFile", state.show_file);
    config_.SaveBool("Pane", "ShowToc", state.show_toc);
    config_.SaveInt("Pane", "FileWidth", static_cast<int>(std::lround(state.file_width)));
    config_.SaveInt("Pane", "TocWidth", static_cast<int>(std::lround(state.toc_width)));
}

SessionService::PaneState SessionService::LoadPaneState(float client_width, float min_width, float default_width) const
{
    PaneState s;
    s.show_file = config_.LoadBool("Pane", "ShowFile", true);
    s.show_toc = config_.LoadBool("Pane", "ShowToc", true);

    const int default_int = static_cast<int>(default_width);
    const int min_int = static_cast<int>(min_width);

    // クライアント幅に基づいて有効な最大ペイン幅を計算する
    int dynamic_max = default_int;
    if (client_width > 0.0f) {
        dynamic_max = std::max(min_int, static_cast<int>(client_width) - min_int);
    }

    // LoadInt は範囲外時に default_int を返すが、その既定値自体が
    // 狭いウィンドウでは dynamic_max を超えうる。最終結果も clamp してから
    // 適用し、SetXxxPaneWidth 側の最小値 clamp に過剰な幅が漏れないようにする。
    const int file_w = std::clamp(config_.LoadInt("Pane", "FileWidth", default_int, min_int, dynamic_max), min_int, dynamic_max);
    const int toc_w = std::clamp(config_.LoadInt("Pane", "TocWidth", default_int, min_int, dynamic_max), min_int, dynamic_max);
    s.file_width = static_cast<float>(file_w);
    s.toc_width = static_cast<float>(toc_w);
    return s;
}

void SessionService::SaveScrollPosition(int node, float scroll_y, float node_y)
{
    const int offset = static_cast<int>(std::lround(scroll_y - node_y));
    config_.SaveInt("Session", "ScrollNode", node);
    config_.SaveInt("Session", "ScrollOffset", offset);
}

SessionService::ScrollPosition SessionService::LoadScrollPosition() const
{
    return {
        .node = config_.LoadInt("Session", "ScrollNode", -1, -1, 1000000),
        .offset = config_.LoadInt("Session", "ScrollOffset", 0, -1000000, 1000000),
    };
}
