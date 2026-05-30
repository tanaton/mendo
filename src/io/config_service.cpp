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

// ini 永続化キー。生文字列を散らさず一覧化し、タイポを静的に検出可能にする。
// 既存の ini ファイル下位互換が要求されるため値の変更は禁止。
namespace {
using namespace std::literals;
constexpr auto kSectionSession = "Session"sv;
constexpr auto kKeyLastFile = "LastFile"sv;
constexpr auto kKeyScrollNode = "ScrollNode"sv;
constexpr auto kKeyScrollOffset = "ScrollOffset"sv;
constexpr auto kSectionPane = "Pane"sv;
constexpr auto kKeyShowFile = "ShowFile"sv;
constexpr auto kKeyShowToc = "ShowToc"sv;
constexpr auto kKeyFileWidth = "FileWidth"sv;
constexpr auto kKeyTocWidth = "TocWidth"sv;
} // namespace

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
    config_.SaveWString(kSectionSession, kKeyLastFile, path);
}

std::pmr::wstring SessionService::LoadLastFilePath() const
{
    std::pmr::wstring path = config_.LoadWString(kSectionSession, kKeyLastFile);
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
    config_.SaveBool(kSectionPane, kKeyShowFile, state.show_file);
    config_.SaveBool(kSectionPane, kKeyShowToc, state.show_toc);
    config_.SaveInt(kSectionPane, kKeyFileWidth, static_cast<int>(std::lround(state.file_width)));
    config_.SaveInt(kSectionPane, kKeyTocWidth, static_cast<int>(std::lround(state.toc_width)));
}

SessionService::PaneState SessionService::LoadPaneState([[maybe_unused]] float client_width, float min_width, float default_width) const
{
    PaneState s;
    s.show_file = config_.LoadBool(kSectionPane, kKeyShowFile, true);
    s.show_toc = config_.LoadBool(kSectionPane, kKeyShowToc, true);

    const int default_int = static_cast<int>(default_width);
    const int min_int = static_cast<int>(min_width);

    // 過剰幅の表示制限は ComputePaneLayout 側 (side 幅を表示時に clamp して MD ペインと
    // スプリッタを画面内に保つ。論理幅は不変) に委ね、ここでは下限のみ保証する。
    // client_width で上限 clamp すると狭いウィンドウ起動時に保存値が min へ潰れ、
    // 終了時の再保存で恒久喪失するため。破損 INI 対策に実用上限のみ設ける。
    constexpr int kPaneWidthLoadMax = 10000;
    const int file_w = config_.LoadInt(kSectionPane, kKeyFileWidth, default_int, min_int, kPaneWidthLoadMax);
    const int toc_w = config_.LoadInt(kSectionPane, kKeyTocWidth, default_int, min_int, kPaneWidthLoadMax);
    s.file_width = static_cast<float>(file_w);
    s.toc_width = static_cast<float>(toc_w);
    return s;
}

void SessionService::SaveScrollPosition(int node, float scroll_y, float node_y)
{
    const int offset = static_cast<int>(std::lround(scroll_y - node_y));
    config_.SaveInt(kSectionSession, kKeyScrollNode, node);
    config_.SaveInt(kSectionSession, kKeyScrollOffset, offset);
}

SessionService::ScrollPosition SessionService::LoadScrollPosition() const
{
    return {
        .node = config_.LoadInt(kSectionSession, kKeyScrollNode, -1, -1, 1000000),
        .offset = config_.LoadInt(kSectionSession, kKeyScrollOffset, 0, -1000000, 1000000),
    };
}
