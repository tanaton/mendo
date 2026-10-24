#pragma once
#include "ini_parser.h"
#include "ui_types.h"
#include <string>
#include <string_view>
#include <filesystem>
#include <memory_resource>

// 設定 (settings.ini) のメモリ上マップとディスク永続化を保持する。
// インスタンスごとに独立した状態を持つので、テストでは新しいインスタンスを使えば良い。
class ConfigService {
public:
    ConfigService() noexcept = default;

    void SetConfigDirOverride(const std::filesystem::path& dir);
    const std::filesystem::path& GetConfigDir() const;
    // 区切り文字や ".." を含む名前は拒否する。
    std::filesystem::path GetConfigPath(std::wstring_view filename) const;

    void Load();
    // 書き込みはアプリ終了時 (WM_DESTROY) に集約する。値が変わっていなければ書かない。
    void Flush();

    void SaveBool(std::string_view section, std::string_view key, bool value);
    bool LoadBool(std::string_view section, std::string_view key, bool default_value) const;

    void SaveInt(std::string_view section, std::string_view key, int value);
    int LoadInt(std::string_view section, std::string_view key, int def, int min_v, int max_v) const;
    // 無い・不正・範囲外なら等倍。
    ZoomLevel LoadZoomLevel(std::string_view section, std::string_view key) const;
    void SaveZoomLevel(std::string_view section, std::string_view key, ZoomLevel zoom);

    void SaveWString(std::string_view section, std::string_view key, std::wstring_view value);
    std::pmr::wstring LoadWString(std::string_view section, std::string_view key) const;

private:
    [[nodiscard]] const std::string* FindValue(std::string_view section, std::string_view key) const;
    void StoreValue(std::string_view section, std::string_view key, std::string value);

    // LocalAppData 配下の設定ディレクトリをプロセス全体で 1 回だけ resolve する。
    // 関数ローカル static (magic static) は C++11 以降 thread-safe な lazy init を保証するため、
    // 別スレッド (並列計測 worker 等) から GetConfigDir() を呼んでも安全。
    static const std::filesystem::path& DefaultConfigDir();

    std::filesystem::path config_dir_override_;
    ini::IniData data_;
    bool dirty_ = false;
};

// セッション状態（最後に開いたファイル、ペイン構成、スクロール位置、言語）の永続化を担当する。
// ConfigService のセクション "Session" / "Pane" / "General" を排他的に扱う薄いラッパ。
class SessionService {
public:
    explicit SessionService(ConfigService& config) noexcept : config_(config)
    {}

    void SaveLastFilePath(std::wstring_view path);
    std::pmr::wstring LoadLastFilePath() const;

    // PaneController との変換は呼び出し側が担う。
    struct PaneState {
        bool show_file = true;
        bool show_toc = true;
        float file_width = SIDE_PANE_DEFAULT_WIDTH;
        float toc_width = SIDE_PANE_DEFAULT_WIDTH;
        ZoomLevel file_zoom;
        ZoomLevel toc_zoom;
    };
    void SavePaneState(const PaneState& state);
    // 幅はズーム倍率を含まない DIP 値として保存・復元する。
    PaneState LoadPaneState() const;

    struct ScrollPosition {
        int node = -1;
        int offset = 0;
    };
    void SaveScrollPosition(int node, float offset);
    ScrollPosition LoadScrollPosition() const;

    // 選んだ言語 (i18n::ConfigLangKey)。空は OS に合わせる。
    void SaveLanguage(std::wstring_view lang_key);
    std::pmr::wstring LoadLanguage() const;

private:
    ConfigService& config_;
};
