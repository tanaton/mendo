#pragma once
#include "ui_types.h"
#include <cstdint>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <string_view>
#include <variant>

// レジストリの REG_DWORD は uint32_t、REG_SZ は std::wstring で持つ。セクション (キー) と値の名前も API に合わせて UTF-16。
using ConfigValue = std::variant<uint32_t, std::wstring>;
using ConfigSection = std::map<std::wstring, ConfigValue, std::less<>>;
using ConfigData = std::map<std::wstring, ConfigSection, std::less<>>;

// 設定を置く HKCU の下のキー (Software\tanaton\mendo)。環境変数 MENDO_SETTINGS_KEY があれば、
// その名前でテスト用の場所 (Software\tanaton\mendo-test\<名前>) にする。
std::wstring DefaultRegistryPath();

// 設定 (セクションごとのキー) のメモリ上マップとレジストリへの永続化を保持する。
// インスタンスごとに独立した状態を持つので、テストでは新しいインスタンスを使えば良い。
class ConfigService {
public:
    // registry_path は HKCU の下のキー。テストは本人の設定と別の場所を渡す。
    explicit ConfigService(std::wstring registry_path = DefaultRegistryPath());

    void Load();
    // 書き込みはアプリ終了時 (WM_DESTROY) に集約する。値が変わっていなければ書かない。
    void Flush();

    void SaveBool(std::wstring_view section, std::wstring_view key, bool value);
    bool LoadBool(std::wstring_view section, std::wstring_view key, bool default_value) const;

    void SaveInt(std::wstring_view section, std::wstring_view key, int value);
    int LoadInt(std::wstring_view section, std::wstring_view key, int def, int min_v, int max_v) const;
    // 無い・不正・範囲外なら等倍。
    ZoomLevel LoadZoomLevel(std::wstring_view section, std::wstring_view key) const;
    void SaveZoomLevel(std::wstring_view section, std::wstring_view key, ZoomLevel zoom);

    void SaveWString(std::wstring_view section, std::wstring_view key, std::wstring_view value);
    std::wstring LoadWString(std::wstring_view section, std::wstring_view key) const;

    // Flush の途中で OS の関数の前に呼ばれ、0 以外を返すとその関数がその値で失敗したことにする。
    // テストが失敗の経路を起こすためにある。
    enum class FlushStep {
        TransactedKey,
        Commit,
        DirectKey
    };
    using FlushHook = std::function<long(FlushStep step)>;
    void SetFlushHookForTest(FlushHook hook);

private:
    [[nodiscard]] const ConfigValue* FindValue(std::wstring_view section, std::wstring_view key) const;
    void StoreValue(std::wstring_view section, std::wstring_view key, ConfigValue value);

    std::wstring registry_path_;
    ConfigData data_;
    // Load (または Flush) の後に値を変えたキー。
    std::map<std::wstring, std::set<std::wstring>> changed_;
    FlushHook flush_hook_;
};

class PaneController;

// セッション状態（最後に開いたファイル、ペイン構成、スクロール位置、言語、ダークモード、本文の倍率、ウィンドウの配置）の永続化。
// ConfigService のセクション "Session" / "Pane" / "General" / "View" / "Window" を排他的に扱う。
namespace session {

void SaveLastFilePath(ConfigService& config, std::wstring_view path);
std::wstring LoadLastFilePath(const ConfigService& config);

// 表示・幅・倍率を保存・復元する。幅はズーム倍率を含まない DIP 値として扱う。
void SavePanes(ConfigService& config, const PaneController& panes);
void LoadPanes(const ConfigService& config, PaneController& panes);

struct ScrollPosition {
    int node = -1;
    int offset = 0;
};
void SaveScrollPosition(ConfigService& config, int node, float offset);
ScrollPosition LoadScrollPosition(const ConfigService& config);

// 選んだ言語 (i18n::ConfigLangKey)。空は OS に合わせる。
void SaveLanguage(ConfigService& config, std::wstring_view lang_key);
std::wstring LoadLanguage(const ConfigService& config);

void SaveDarkMode(ConfigService& config, bool dark);
bool LoadDarkMode(const ConfigService& config);

void SaveZoomLevel(ConfigService& config, ZoomLevel zoom);
ZoomLevel LoadZoomLevel(const ConfigService& config);

struct WindowPlacement {
    int x = 0;
    int y = 0;
    int width = 0;
    int height = 0;
    bool maximized = false;
};
void SaveWindowPlacement(ConfigService& config, const WindowPlacement& placement);
// 未保存なら width と height が 0。
WindowPlacement LoadWindowPlacement(const ConfigService& config);

} // namespace session
