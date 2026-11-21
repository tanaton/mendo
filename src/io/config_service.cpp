#include "config_service.h"
#include "file_io.h"
#include "pane_layout.h"
#include "utility.h"
#include "win32_util.h"
#include "win_handle.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <optional>
#include <span>
#include <vector>
#include <windows.h>
#include <ktmw32.h>

// セクションはレジストリのキー、キーは値の名前になる。生文字列を散らさず一覧化し、タイポを静的に検出する。
namespace {
using namespace std::literals;
constexpr auto kSectionSession = L"Session"sv;
constexpr auto kKeyLastFile = L"LastFile"sv;
constexpr auto kKeyScrollNode = L"ScrollNode"sv;
constexpr auto kKeyScrollOffset = L"ScrollOffset"sv;
constexpr auto kSectionPane = L"Pane"sv;
constexpr auto kKeyShowFile = L"ShowFile"sv;
constexpr auto kKeyShowToc = L"ShowToc"sv;
constexpr auto kKeyFileWidth = L"FileWidth"sv;
constexpr auto kKeyTocWidth = L"TocWidth"sv;
constexpr auto kKeyFileZoomLevel = L"FileZoomLevel"sv;
constexpr auto kKeyTocZoomLevel = L"TocZoomLevel"sv;
constexpr auto kSectionGeneral = L"General"sv;
constexpr auto kKeyLanguage = L"Language"sv;
constexpr auto kSectionView = L"View"sv;
constexpr auto kKeyDarkMode = L"DarkMode"sv;
constexpr auto kKeyZoomLevel = L"ZoomLevel"sv;
constexpr auto kSectionWindow = L"Window"sv;
constexpr auto kKeyWindowX = L"X"sv;
constexpr auto kKeyWindowY = L"Y"sv;
constexpr auto kKeyWindowWidth = L"Width"sv;
constexpr auto kKeyWindowHeight = L"Height"sv;
constexpr auto kKeyWindowMaximized = L"Maximized"sv;

// 組で意味を持つ値のセクション。Flush で 1 つでも変えていたらセクションごと書く。
// キーごとに重ねると、別のプロセスの値と混ざる (bar.md に foo.md のスクロール位置が付くなど)。
constexpr std::wstring_view kWholeSections[] = { kSectionSession, kSectionWindow };

// 設定はレジストリに置き、終了時にトランザクション (RegCreateKeyTransactedW) でまとめて書く (ユーザーが選んだ)。
// 読む側はコミット前の値を見ないので Session / Window の組が書きかけで混ざらず、ファイルの丸ごと置き換えに要った
// 一時ファイル・FlushFileBuffers・読み書きを囲むプロセス間のロックも要らない。
// 前の版の settings.ini は取り込まない。項目が少なく、ユーザーが設定し直せば足りる (ユーザーが選んだ)。
constexpr auto kRegistryPath = L"Software\\tanaton\\mendo"sv;
// スクリーンショットや計測で本人の設定を書き換えないための場所 (ユーザーが選んだ)。引数でなく環境変数で渡すのは、
// Ctrl+N の新しいウィンドウ (CreateProcessW) にも環境ごと自動で引き継がれるため。本番のキーの下に置くと
// セクションとして読まれるので、隣のキーにする。
constexpr auto kTestRegistryParent = L"Software\\tanaton\\mendo-test\\"sv;
} // namespace

std::wstring DefaultRegistryPath()
{
    if (const std::wstring name = QueryPath([](wchar_t* b, UINT n) static { return GetEnvironmentVariableW(L"MENDO_SETTINGS_KEY", b, n); });
        !name.empty()) {
        return std::wstring(kTestRegistryParent) + name;
    }
    return std::wstring(kRegistryPath);
}

ConfigService::ConfigService(std::wstring registry_path) : registry_path_(std::move(registry_path))
{
}

void ConfigService::SetFlushHookForTest(FlushHook hook)
{
    flush_hook_ = std::move(hook);
}

namespace {
// REG_DWORD と REG_SZ のほかの型 (regedit で書き換えたものなど) は読まない。
std::optional<ConfigValue> ToConfigValue(DWORD type, const BYTE* data, DWORD size)
{
    if (type == REG_DWORD && size == sizeof(uint32_t)) {
        uint32_t value = 0;
        std::memcpy(&value, data, sizeof(value));
        return value;
    }
    if (type == REG_SZ) {
        std::wstring value(size / sizeof(wchar_t), L'\0');
        std::memcpy(value.data(), data, value.size() * sizeof(wchar_t));
        // 終端の NUL は書いたプログラムしだいで有ったり無かったりする。
        while (!value.empty() && value.back() == L'\0') {
            value.pop_back();
        }
        return value;
    }
    return std::nullopt;
}

// 値を 1 つずつ読むと、間に別のプロセスのコミットが入って Session / Window の組が混ざるので、名前を列挙してから
// RegQueryMultipleValuesW で 1 度に読む (資料が static key の値をまとめて読むと明記している)。列挙との間に値が
// 消えたり大きくなったりしたら読み直す。
ConfigSection ReadSection(HKEY key)
{
    constexpr int kReadAttempts = 3;
    std::vector<BYTE> buf(1024);
    for (int attempt = 0; attempt < kReadAttempts; ++attempt) {
        DWORD count = 0;
        DWORD max_name = 0;
        if (RegQueryInfoKeyW(key, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, &count, &max_name, nullptr, nullptr, nullptr) != ERROR_SUCCESS) {
            return {};
        }
        std::vector<std::wstring> names;
        names.reserve(count);
        std::wstring name(max_name + 1, L'\0');
        for (DWORD i = 0; i < count; ++i) {
            DWORD len = static_cast<DWORD>(name.size());
            if (RegEnumValueW(key, i, name.data(), &len, nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS) {
                break;
            }
            names.emplace_back(name.data(), len);
        }
        if (names.empty()) {
            return {};
        }
        std::vector<VALENTW> entries(names.size());
        for (size_t i = 0; i < names.size(); ++i) {
            entries[i].ve_valuename = names[i].data();
        }
        DWORD size = static_cast<DWORD>(buf.size());
        const LSTATUS status = RegQueryMultipleValuesW(key, entries.data(), static_cast<DWORD>(entries.size()), reinterpret_cast<LPWSTR>(buf.data()), &size);
        if (status == ERROR_MORE_DATA) {
            buf.resize(size);
            continue;
        }
        if (status == ERROR_FILE_NOT_FOUND) {
            continue;
        }
        if (status != ERROR_SUCCESS) {
            return {};
        }
        ConfigSection section;
        for (size_t i = 0; i < names.size(); ++i) {
            const VALENTW& e = entries[i];
            if (auto value = ToConfigValue(e.ve_type, reinterpret_cast<const BYTE*>(e.ve_valueptr), e.ve_valuelen)) {
                section.emplace(std::move(names[i]), std::move(*value));
            }
        }
        return section;
    }
    return {};
}

ConfigData ReadTree(HKEY root)
{
    ConfigData data;
    // キーの名前は 255 文字まで。
    wchar_t name[256];
    for (DWORD i = 0;; ++i) {
        DWORD len = static_cast<DWORD>(std::size(name));
        if (RegEnumKeyExW(root, i, name, &len, nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS) {
            break;
        }
        UniqueRegKey section;
        if (RegOpenKeyExW(root, name, 0, KEY_QUERY_VALUE, section.put()) != ERROR_SUCCESS) {
            continue;
        }
        if (ConfigSection values = ReadSection(section.get()); !values.empty()) {
            data.emplace(std::wstring(name, len), std::move(values));
        }
    }
    return data;
}

struct SectionWrite {
    std::wstring path;
    // data_ の中の名前と値。
    std::vector<const ConfigSection::value_type*> values;
};

LSTATUS SetValue(HKEY key, const ConfigSection::value_type& entry)
{
    const wchar_t* name = entry.first.c_str();
    return std::visit(overloaded{
                          [&](uint32_t n) {
        return RegSetValueExW(key, name, 0, REG_DWORD, reinterpret_cast<const BYTE*>(&n), sizeof(n));
    },
                          [&](const std::wstring& s) {
        const auto bytes = static_cast<DWORD>((s.size() + 1) * sizeof(wchar_t));
        return RegSetValueExW(key, name, 0, REG_SZ, reinterpret_cast<const BYTE*>(s.c_str()), bytes);
    },
                      },
                      entry.second);
}

using FlushHook = ConfigService::FlushHook;
using FlushStep = ConfigService::FlushStep;

LSTATUS RunHook(const FlushHook& hook, FlushStep step)
{
    return hook ? hook(step) : ERROR_SUCCESS;
}

// tx が無ければトランザクション無しで書く。コミットはしない。
// トランザクションはどうせ巻き戻すので最初の失敗で止め、トランザクション無しならほかのセクションは書く。
LSTATUS WriteSections(HANDLE tx, std::span<const SectionWrite> writes, const FlushHook& hook)
{
    LSTATUS result = ERROR_SUCCESS;
    for (const auto& w : writes) {
        UniqueRegKey key;
        LSTATUS status = RunHook(hook, tx ? FlushStep::TransactedKey : FlushStep::DirectKey);
        if (status == ERROR_SUCCESS) {
            status = tx ? RegCreateKeyTransactedW(HKEY_CURRENT_USER, w.path.c_str(), 0, nullptr, REG_OPTION_NON_VOLATILE, KEY_SET_VALUE, nullptr,
                                                  key.put(), nullptr, tx, nullptr)
                        : RegCreateKeyExW(HKEY_CURRENT_USER, w.path.c_str(), 0, nullptr, REG_OPTION_NON_VOLATILE, KEY_SET_VALUE, nullptr, key.put(), nullptr);
        }
        for (size_t i = 0; status == ERROR_SUCCESS && i < w.values.size(); ++i) {
            status = SetValue(key.get(), *w.values[i]);
        }
        if (status != ERROR_SUCCESS) {
            if (tx) {
                return status;
            }
            result = status;
        }
    }
    return result;
}

LSTATUS WriteTransacted(std::span<const SectionWrite> writes, const FlushHook& hook)
{
    const UniqueHandle tx(CreateTransaction(nullptr, nullptr, 0, 0, 0, 0, nullptr));
    if (!tx) {
        return static_cast<LSTATUS>(GetLastError());
    }
    LSTATUS status = WriteSections(tx.get(), writes, hook);
    if (status == ERROR_SUCCESS) {
        status = RunHook(hook, FlushStep::Commit);
    }
    if (status != ERROR_SUCCESS) {
        RollbackTransaction(tx.get());
        return status;
    }
    return CommitTransaction(tx.get()) ? ERROR_SUCCESS : static_cast<LSTATUS>(GetLastError());
}

bool WriteValues(std::span<const SectionWrite> writes, const FlushHook& hook)
{
    // 相手のトランザクションは値を十数個書くだけですぐ終わるので、短い間隔で数回書き直せば足りる。
    constexpr int kTransactedAttempts = 4;
    constexpr DWORD kRetryDelayMs = 10;
    for (int attempt = 0; attempt < kTransactedAttempts; ++attempt) {
        if (attempt > 0) {
            Sleep(kRetryDelayMs);
        }
        const LSTATUS status = WriteTransacted(writes, hook);
        if (status == ERROR_SUCCESS) {
            return true;
        }
        // 別の mendo が同じキーを書いている最中 (ERROR_TRANSACTIONAL_CONFLICT) か、書いている間に regedit などが
        // トランザクション無しで書いて中止された (ERROR_TRANSACTION_ALREADY_ABORTED)。ほかの失敗は書き直しても変わらない。
        if (status != ERROR_TRANSACTIONAL_CONFLICT && status != ERROR_TRANSACTION_ALREADY_ABORTED) {
            break;
        }
    }
    // トランザクションが使えない (FSLogix のプロファイルで ERROR_RM_NOT_ACTIVE になった報告がある) か、衝突が続いた。
    // トランザクション無しで書く。まれに Session / Window が別のプロセスの値と混ざるのは受け入れる。
    return WriteSections(nullptr, writes, hook) == ERROR_SUCCESS;
}
} // namespace

void ConfigService::Load()
{
    data_.clear();
    changed_.clear();
    UniqueRegKey root;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, registry_path_.c_str(), 0, KEY_READ, root.put()) == ERROR_SUCCESS) {
        data_ = ReadTree(root.get());
    }
}

void ConfigService::Flush()
{
    if (changed_.empty()) {
        return;
    }
    std::vector<SectionWrite> writes;
    writes.reserve(changed_.size());
    for (const auto& [section, keys] : changed_) {
        const bool whole = std::ranges::contains(kWholeSections, section);
        SectionWrite& w = writes.emplace_back();
        w.path = registry_path_ + L'\\' + section;
        for (const auto& entry : data_.find(section)->second) {
            if (whole || keys.contains(entry.first)) {
                w.values.push_back(&entry);
            }
        }
    }
    if (WriteValues(writes, flush_hook_)) {
        changed_.clear();
    }
}

const ConfigValue* ConfigService::FindValue(std::wstring_view section, std::wstring_view key) const
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

void ConfigService::StoreValue(std::wstring_view section, std::wstring_view key, ConfigValue value)
{
    if (const auto* current = FindValue(section, key); current && *current == value) {
        return;
    }
    data_[std::wstring(section)][std::wstring(key)] = std::move(value);
    // 終了時は変えていない値も Save を呼ぶので、値が変わったときだけ記録する。
    changed_[std::wstring(section)].emplace(key);
}

void ConfigService::SaveBool(std::wstring_view section, std::wstring_view key, bool value)
{
    StoreValue(section, key, uint32_t{ value ? 1u : 0u });
}

bool ConfigService::LoadBool(std::wstring_view section, std::wstring_view key, bool default_value) const
{
    const auto* val = std::get_if<uint32_t>(FindValue(section, key));
    return val ? (*val != 0) : default_value;
}

void ConfigService::SaveInt(std::wstring_view section, std::wstring_view key, int value)
{
    // 負の値は 2 の補数のまま REG_DWORD に入れる。
    StoreValue(section, key, static_cast<uint32_t>(value));
}

int ConfigService::LoadInt(std::wstring_view section, std::wstring_view key, int def, int min_v, int max_v) const
{
    const auto* val = std::get_if<uint32_t>(FindValue(section, key));
    if (!val) {
        return def;
    }
    const int result = static_cast<int>(*val);
    if (result < min_v || result > max_v) {
        return def;
    }
    return result;
}

ZoomLevel ConfigService::LoadZoomLevel(std::wstring_view section, std::wstring_view key) const
{
    ZoomLevel zoom;
    zoom.SetIndex(LoadInt(section, key, ZOOM_DEFAULT_INDEX, 0, ZOOM_STEP_COUNT - 1));
    return zoom;
}

void ConfigService::SaveZoomLevel(std::wstring_view section, std::wstring_view key, ZoomLevel zoom)
{
    SaveInt(section, key, zoom.Index());
}

void ConfigService::SaveWString(std::wstring_view section, std::wstring_view key, std::wstring_view value)
{
    StoreValue(section, key, std::wstring(value));
}

std::wstring ConfigService::LoadWString(std::wstring_view section, std::wstring_view key) const
{
    const auto* val = std::get_if<std::wstring>(FindValue(section, key));
    return val ? *val : std::wstring{};
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
    // ネットワークドライブは確かめずに渡し、無ければ worker での読み込みの失敗に任せる。
    if (path_util::IsOnNetworkDrive(path)) {
        return path;
    }
    if (GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES) {
        return {};
    }
    return path;
}

namespace {
struct PaneKeys {
    PaneTarget target;
    std::wstring_view show;
    std::wstring_view width;
    std::wstring_view zoom;
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
    // 過剰幅の表示制限は PaneController::ComputeLayout 側 (side 幅を表示時に clamp して MD ペインとスプリッタを
    // 画面内に保つ。論理幅は不変) に委ね、ここでは下限のみ保証する。ウィンドウ幅で上限 clamp すると
    // 狭いウィンドウ起動時に保存値が min へ潰れ、終了時の再保存で恒久喪失するため。壊れた値に備えて実用上限のみ設ける。
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
