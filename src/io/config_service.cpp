#include "config_service.h"
#include "file_io.h"
#include "pane_layout.h"
#include "string_convert.h"
#include "utf8_codec.h"
#include "win_handle.h"
#include <algorithm>
#include <charconv>
#include <cmath>
#include <optional>
#include <shlobj.h>
#include <windows.h>

namespace ini {

namespace {
constexpr std::string_view TrimBlanks(std::string_view s) noexcept
{
    const size_t first = s.find_first_not_of(" \t");
    if (first == std::string_view::npos) {
        return {};
    }
    return s.substr(first, s.find_last_not_of(" \t") - first + 1);
}
} // namespace

IniData Parse(std::string_view text)
{
    IniData data;
    std::string current_section;

    size_t pos = 0;
    while (pos < text.size()) {
        size_t eol = text.find_first_of("\r\n", pos);
        if (eol == std::string_view::npos) {
            eol = text.size();
        }
        const std::string_view line = TrimBlanks(text.substr(pos, eol - pos));

        pos = eol;
        if (pos < text.size() && text[pos] == '\r') {
            ++pos;
        }
        if (pos < text.size() && text[pos] == '\n') {
            ++pos;
        }

        if (line.empty() || line[0] == ';' || line[0] == '#') {
            continue;
        }

        if (line[0] == '[') {
            const size_t close = line.find(']', 1);
            if (close != std::string_view::npos) {
                current_section = std::string(line.substr(1, close - 1));
            }
            else {
                // 未終端ブラケットは壊れたセクション開始。後続キーが直前セクションへ
                // 誤混入するのを防ぐため無名セクションへ退避する。
                current_section.clear();
            }
            continue;
        }

        const size_t eq = line.find('=');
        if (eq == std::string_view::npos) {
            continue;
        }

        const std::string_view key = TrimBlanks(line.substr(0, eq));
        if (key.empty()) {
            continue;
        }
        data[current_section][std::string(key)] = std::string(TrimBlanks(line.substr(eq + 1)));
    }

    return data;
}

std::string Serialize(const IniData& data)
{
    std::string result;
    bool first_section = true;

    for (const auto& [section, kvs] : data) {
        if (kvs.empty()) {
            continue;
        }
        if (!first_section) {
            result += '\n';
        }
        first_section = false;

        if (!section.empty()) {
            result += '[';
            result += section;
            result += "]\n";
        }

        for (const auto& [key, value] : kvs) {
            result += key;
            result += '=';
            result += value;
            result += '\n';
        }
    }

    return result;
}

} // namespace ini

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
constexpr auto kSectionView = "View"sv;
constexpr auto kKeyDarkMode = "DarkMode"sv;
constexpr auto kKeyZoomLevel = "ZoomLevel"sv;
constexpr auto kSectionWindow = "Window"sv;
constexpr auto kKeyWindowX = "X"sv;
constexpr auto kKeyWindowY = "Y"sv;
constexpr auto kKeyWindowWidth = "Width"sv;
constexpr auto kKeyWindowHeight = "Height"sv;
constexpr auto kKeyWindowMaximized = "Maximized"sv;

// 組で意味を持つ値のセクション。Flush で 1 つでも変えていたらセクションごと書く。
// キーごとに重ねると、別のプロセスの値と混ざる (bar.md に foo.md のスクロール位置が付くなど)。
constexpr std::string_view kWholeSections[] = { kSectionSession, kSectionWindow };
} // namespace

namespace {
std::filesystem::path DefaultConfigDir()
{
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
    // 必ず絶対パスを返し、設定と図のキャッシュに「置き場所が無い」場合を持ち込まない。相対パスだとカレントディレクトリの
    // 下を読み書きしてしまう。temp_directory_path は一時フォルダが無いと空を返すが、GetTempPathW は存在を確かめない。
    wchar_t dir[MAX_PATH + 1]{};
    GetTempPathW(MAX_PATH + 1, dir);
    // TMP / TEMP / USERPROFILE がすべて空だと GetTempPathW も絶対パスを返さない。書けなくても保存されないだけ。
    if (!std::filesystem::path(dir).is_absolute()) {
        GetWindowsDirectoryW(dir, MAX_PATH + 1);
    }
    return std::filesystem::path(dir) / L"mendo";
}
} // namespace

ConfigService::ConfigService() : config_dir_(DefaultConfigDir())
{
}

void ConfigService::SetConfigDir(const std::filesystem::path& dir)
{
    config_dir_ = dir;
}

std::filesystem::path ConfigService::GetConfigPath(std::wstring_view filename) const
{
    return config_dir_ / filename;
}

namespace {
// 無いファイルは空として返し、読めなかったときだけ nullopt を返す。読めなかったのを空と見なして書くと、他の値を消してしまう。
std::optional<ini::IniData> ReadIniFile(const std::filesystem::path& path)
{
    DWORD error = ERROR_SUCCESS;
    // 壊れた巨大ファイルで確保に失敗しないよう、上限は設定として十分な大きさに抑える。
    constexpr LONGLONG kMaxSize = 1 << 20;
    auto opened = OpenFileForReadShared(path.c_str(), FILE_SHARE_READ, kMaxSize, &error);
    if (!opened) {
        if (IsFileMissingError(error)) {
            return ini::IniData{};
        }
        return std::nullopt;
    }
    std::string buf(opened->size, '\0');
    if (!ReadExact(opened->handle.get(), buf.data(), buf.size())) {
        return std::nullopt;
    }
    std::string_view content = buf;
    // 手編集ツールが付ける UTF-8 BOM を除去しないと先頭セクションが認識されない
    if (content.starts_with(utf8_codec::kBom)) {
        content.remove_prefix(utf8_codec::kBom.size());
    }
    return ini::Parse(content);
}

// 前の版と同じミューテックスの名前になるよう "settings" のまま変えない。
constexpr std::wstring_view kSettingsLockKind = L"settings";
} // namespace

void ConfigService::Load()
{
    const auto ini_path = GetConfigPath(kSettingsFile);
    const InterProcessFileLock lock(kSettingsLockKind, ini_path);
    if (auto data = ReadIniFile(ini_path)) {
        data_ = std::move(*data);
    }
    changed_.clear();
}

void ConfigService::OverlayChanges(ini::IniData& target) const
{
    for (const auto& [section, keys] : changed_) {
        const auto& mine = data_.find(section)->second;
        auto& dst = target[section];
        if (std::ranges::contains(kWholeSections, section)) {
            // 他のバージョンが足したキーを消さないよう、置き換えずに重ねる。
            for (const auto& [key, value] : mine) {
                dst[key] = value;
            }
        }
        else {
            for (const auto& key : keys) {
                dst[key] = mine.find(key)->second;
            }
        }
    }
}

void ConfigService::Flush()
{
    if (changed_.empty()) {
        return;
    }
    const auto ini_path = GetConfigPath(kSettingsFile);
    std::error_code ec;
    std::filesystem::create_directories(ini_path.parent_path(), ec);
    if (ec) {
        return;
    }

    const InterProcessFileLock lock(kSettingsLockKind, ini_path);
    // 他のウィンドウが先に閉じて書いた値を、起動時に読んだ古い値で戻さない。読めなければ従来どおり丸ごと書く。
    auto merged = ReadIniFile(ini_path).value_or(data_);
    OverlayChanges(merged);
    const std::string content = ini::Serialize(merged);
    if (AtomicWriteAllBytes(ini_path, content.data(), content.size())) {
        data_ = std::move(merged);
        changed_.clear();
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
    // 終了時は変えていない値も Save を呼ぶので、値が変わったときだけ記録する。
    changed_[std::string(section)].emplace(key);
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

std::wstring ConfigService::LoadWString(std::string_view section, std::string_view key) const
{
    const auto* val = FindValue(section, key);
    return val ? string_convert::Utf8ToWide(*val) : std::wstring{};
}

void session::SaveLastFilePath(ConfigService& config, std::wstring_view path)
{
    if (path.empty()) {
        return;
    }
    config.SaveWString(kSectionSession, kKeyLastFile, path);
}

std::wstring session::LoadLastFilePath(const ConfigService& config)
{
    std::wstring path = config.LoadWString(kSectionSession, kKeyLastFile);
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
    { PaneTarget::Toc,  kKeyShowToc,  kKeyTocWidth,  kKeyTocZoomLevel  },
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

std::wstring session::LoadLanguage(const ConfigService& config)
{
    return config.LoadWString(kSectionGeneral, kKeyLanguage);
}

void session::SaveDarkMode(ConfigService& config, bool dark)
{
    config.SaveBool(kSectionView, kKeyDarkMode, dark);
}

bool session::LoadDarkMode(const ConfigService& config)
{
    return config.LoadBool(kSectionView, kKeyDarkMode, false);
}

void session::SaveZoomLevel(ConfigService& config, ZoomLevel zoom)
{
    config.SaveZoomLevel(kSectionView, kKeyZoomLevel, zoom);
}

ZoomLevel session::LoadZoomLevel(const ConfigService& config)
{
    return config.LoadZoomLevel(kSectionView, kKeyZoomLevel);
}

void session::SaveWindowPlacement(ConfigService& config, const WindowPlacement& placement)
{
    config.SaveInt(kSectionWindow, kKeyWindowX, placement.x);
    config.SaveInt(kSectionWindow, kKeyWindowY, placement.y);
    config.SaveInt(kSectionWindow, kKeyWindowWidth, placement.width);
    config.SaveInt(kSectionWindow, kKeyWindowHeight, placement.height);
    config.SaveBool(kSectionWindow, kKeyWindowMaximized, placement.maximized);
}

session::WindowPlacement session::LoadWindowPlacement(const ConfigService& config)
{
    const int w = config.LoadInt(kSectionWindow, kKeyWindowWidth, 0, 100, 100000);
    const int h = config.LoadInt(kSectionWindow, kKeyWindowHeight, 0, 100, 100000);
    if (w == 0 || h == 0) {
        return {};
    }
    return {
        .x = config.LoadInt(kSectionWindow, kKeyWindowX, 0, -100000, 100000),
        .y = config.LoadInt(kSectionWindow, kKeyWindowY, 0, -100000, 100000),
        .width = w,
        .height = h,
        .maximized = config.LoadBool(kSectionWindow, kKeyWindowMaximized, false),
    };
}
