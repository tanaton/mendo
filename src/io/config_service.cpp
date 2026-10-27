#include "config_service.h"
#include "file_io.h"
#include "pane_layout.h"
#include "string_convert.h"
#include "utf8_codec.h"
#include "win_handle.h"
#include <charconv>
#include <cmath>
#include <shlobj.h>
#include <windows.h>


// ini 永続化キー。生文字列を散らさず一覧化し、タイポを静的に検出可能にする。
// 既存の ini ファイル下位互換が要求されるため値の変更は禁止。
namespace {
using namespace std::literals;
constexpr auto kSettingsFile = L"settings.ini"sv;
constexpr auto kSectionSession = "Session"sv;
constexpr auto kKeyLastFile = "LastFile"sv;
constexpr auto kKeyScrollNode = "ScrollNode"sv;
constexpr auto kKeyScrollOffset = "ScrollOffset"sv;
constexpr auto kSectionPane = "Pane"sv;
constexpr auto kKeyShowFile = "ShowFile"sv;
constexpr auto kKeyShowToc = "ShowToc"sv;
constexpr auto kKeyFileWidth = "FileWidth"sv;
constexpr auto kKeyTocWidth = "TocWidth"sv;
constexpr auto kKeyFileZoomLevel = "FileZoomLevel"sv;
constexpr auto kKeyTocZoomLevel = "TocZoomLevel"sv;
constexpr auto kSectionGeneral = "General"sv;
constexpr auto kKeyLanguage = "Language"sv;
} // namespace

void ConfigService::SetConfigDirOverride(const std::filesystem::path& dir)
{
    config_dir_override_ = dir;
}

const std::filesystem::path& ConfigService::DefaultConfigDir()
{
    // 失敗時は空 path のままキャッシュされ、GetConfigPath で空チェックされる。
    static const std::filesystem::path kDir = []() static noexcept -> std::filesystem::path {
        // SHGetKnownFolderPath は初回に windows.storage.dll のロード等で ms 単位かかるため、環境変数を優先する。
        wchar_t env[MAX_PATH];
        const DWORD len = GetEnvironmentVariableW(L"LOCALAPPDATA", env, MAX_PATH);
        if (len < MAX_PATH) {
            std::filesystem::path base(std::wstring_view(env, len));
            if (base.is_absolute()) {
                base /= L"mendo";
                return base;
            }
        }
        // 失敗時も解放が必要なので RAII で受ける。
        UniqueCoTaskMemStr appdata;
        if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, appdata.put()))) {
            return std::filesystem::path(appdata.get()) / L"mendo";
        }
        return {};
    }();
    return kDir;
}

const std::filesystem::path& ConfigService::GetConfigDir() const
{
    if (!config_dir_override_.empty()) {
        return config_dir_override_;
    }
    return DefaultConfigDir();
}

std::filesystem::path ConfigService::GetConfigPath(std::wstring_view filename) const
{
    const auto& dir = GetConfigDir();
    return dir.empty() ? std::filesystem::path{} : dir / filename;
}

void ConfigService::Load()
{
    const auto ini_path = GetConfigPath(kSettingsFile);
    if (ini_path.empty()) {
        return;
    }
    const auto buf = ReadAllBytes(ini_path.c_str());
    if (buf.empty()) {
        return;
    }
    std::string_view content(reinterpret_cast<const char*>(buf.data()), buf.size());
    // 手編集ツールが付ける UTF-8 BOM を除去しないと先頭セクションが認識されない
    if (content.starts_with(utf8_codec::kBom)) {
        content.remove_prefix(utf8_codec::kBom.size());
    }
    data_ = ini::Parse(content);
    dirty_ = false;
}

void ConfigService::Flush()
{
    const auto ini_path = GetConfigPath(kSettingsFile);
    if (!dirty_ || ini_path.empty()) {
        return;
    }
    std::error_code ec;
    std::filesystem::create_directories(ini_path.parent_path(), ec);
    if (ec) {
        return;
    }

    const std::string content = ini::Serialize(data_);
    if (AtomicWriteAllBytes(ini_path, content.data(), content.size())) {
        dirty_ = false;
    }
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

void ConfigService::StoreValue(std::string_view section, std::string_view key, std::string value)
{
    if (const auto* current = FindValue(section, key); current && *current == value) {
        return;
    }
    data_[std::string(section)][std::string(key)] = std::move(value);
    dirty_ = true;
}

void ConfigService::SaveBool(std::string_view section, std::string_view key, bool value)
{
    StoreValue(section, key, value ? "1" : "0");
}

bool ConfigService::LoadBool(std::string_view section, std::string_view key, bool default_value) const
{
    const auto* val = FindValue(section, key);
    return val ? (*val == "1") : default_value;
}

void ConfigService::SaveInt(std::string_view section, std::string_view key, int value)
{
    StoreValue(section, key, std::to_string(value));
}

int ConfigService::LoadInt(std::string_view section, std::string_view key, int def, int min_v, int max_v) const
{
    const auto* val = FindValue(section, key);
    if (!val) {
        return def;
    }
    int result = def;
    const auto [ptr, ec] = std::from_chars(val->data(), val->data() + val->size(), result);
    // 末尾にゴミが残る部分パース ("300px" 等) は不正値として既定値へ倒す。
    if (ec != std::errc{} || ptr != val->data() + val->size()) {
        return def;
    }
    if (result < min_v || result > max_v) {
        return def;
    }
    return result;
}

ZoomLevel ConfigService::LoadZoomLevel(std::string_view section, std::string_view key) const
{
    ZoomLevel zoom;
    zoom.SetIndex(LoadInt(section, key, ZOOM_DEFAULT_INDEX, 0, ZOOM_STEP_COUNT - 1));
    return zoom;
}

void ConfigService::SaveZoomLevel(std::string_view section, std::string_view key, ZoomLevel zoom)
{
    SaveInt(section, key, zoom.Index());
}

void ConfigService::SaveWString(std::string_view section, std::string_view key, std::wstring_view value)
{
    StoreValue(section, key, string_convert::WideToUtf8(value));
}

std::pmr::wstring ConfigService::LoadWString(std::string_view section, std::string_view key) const
{
    const auto* val = FindValue(section, key);
    return val ? string_convert::Utf8ToWide(*val) : std::pmr::wstring{};
}

void session::SaveLastFilePath(ConfigService& config, std::wstring_view path)
{
    if (path.empty()) {
        return;
    }
    config.SaveWString(kSectionSession, kKeyLastFile, path);
}

std::pmr::wstring session::LoadLastFilePath(const ConfigService& config)
{
    std::pmr::wstring path = config.LoadWString(kSectionSession, kKeyLastFile);
    if (path.empty()) {
        return {};
    }
    if (!path_util::IsLocalPath(path)) {
        return {};
    }
    // 割り当てたネットワークドライブは、切断されていると存在の確認だけでウィンドウを出す前に数秒止まる。
    // 確かめずに渡し、無ければ worker での読み込みの失敗に任せる。
    if (path_util::StartsWithDrive(path)) {
        const wchar_t root[] = { path[0], L':', L'\\', L'\0' };
        if (GetDriveTypeW(root) == DRIVE_REMOTE) {
            return path;
        }
    }
    if (GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES) {
        return {};
    }
    return path;
}

namespace {
struct PaneKeys {
    PaneTarget target;
    std::string_view show;
    std::string_view width;
    std::string_view zoom;
};
constexpr PaneKeys kPaneKeys[] = {
    { PaneTarget::File, kKeyShowFile, kKeyFileWidth, kKeyFileZoomLevel },
    { PaneTarget::Toc, kKeyShowToc, kKeyTocWidth, kKeyTocZoomLevel },
};
} // namespace

void session::SavePanes(ConfigService& config, const PaneController& panes)
{
    for (const auto& k : kPaneKeys) {
        config.SaveBool(kSectionPane, k.show, panes.IsSidePaneVisible(k.target));
        config.SaveInt(kSectionPane, k.width, static_cast<int>(std::lround(panes.GetSidePaneWidth(k.target))));
        config.SaveZoomLevel(kSectionPane, k.zoom, panes.SidePaneZoom(k.target));
    }
}

void session::LoadPanes(const ConfigService& config, PaneController& panes)
{
    constexpr int default_int = static_cast<int>(SIDE_PANE_DEFAULT_WIDTH);
    constexpr int min_int = static_cast<int>(SIDE_PANE_MIN_WIDTH);
    // 過剰幅の表示制限は PaneController::ComputeLayout 側 (side 幅を表示時に clamp して MD ペインと
    // スプリッタを画面内に保つ。論理幅は不変) に委ね、ここでは下限のみ保証する。
    // ウィンドウ幅で上限 clamp すると狭いウィンドウ起動時に保存値が min へ潰れ、
    // 終了時の再保存で恒久喪失するため。破損 INI 対策に実用上限のみ設ける。
    constexpr int kPaneWidthLoadMax = 10000;
    for (const auto& k : kPaneKeys) {
        panes.SetSidePaneVisible(k.target, config.LoadBool(kSectionPane, k.show, true));
        panes.SetSidePaneWidth(k.target, static_cast<float>(config.LoadInt(kSectionPane, k.width, default_int, min_int, kPaneWidthLoadMax)));
        panes.SidePaneZoom(k.target) = config.LoadZoomLevel(kSectionPane, k.zoom);
    }
}

void session::SaveScrollPosition(ConfigService& config, int node, float offset)
{
    config.SaveInt(kSectionSession, kKeyScrollNode, node);
    config.SaveInt(kSectionSession, kKeyScrollOffset, static_cast<int>(std::lround(offset)));
}

session::ScrollPosition session::LoadScrollPosition(const ConfigService& config)
{
    return {
        .node = config.LoadInt(kSectionSession, kKeyScrollNode, -1, -1, 1000000),
        .offset = config.LoadInt(kSectionSession, kKeyScrollOffset, 0, -1000000, 1000000),
    };
}

void session::SaveLanguage(ConfigService& config, std::wstring_view lang_key)
{
    config.SaveWString(kSectionGeneral, kKeyLanguage, lang_key);
}

std::pmr::wstring session::LoadLanguage(const ConfigService& config)
{
    return config.LoadWString(kSectionGeneral, kKeyLanguage);
}
