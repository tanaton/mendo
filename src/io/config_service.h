#pragma once
#include "ini_parser.h"
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
    std::filesystem::path GetConfigDir() const;
    // 区切り文字や ".." を含む名前は拒否する。
    std::filesystem::path GetConfigPath(std::wstring_view filename) const;

    void Load();
    // 書き込みはアプリ終了時 (WM_DESTROY) に集約する。
    void Flush();
    void Clear() noexcept;

    void SaveBool(std::string_view section, std::string_view key, bool value);
    bool LoadBool(std::string_view section, std::string_view key, bool default_value = false) const;

    void SaveInt(std::string_view section, std::string_view key, int value);
    int LoadInt(std::string_view section, std::string_view key, int def, int min_v, int max_v) const;

    void SaveWString(std::string_view section, std::string_view key, std::wstring_view value);
    std::pmr::wstring LoadWString(std::string_view section, std::string_view key) const;

private:
    [[nodiscard]] const std::string* FindValue(std::string_view section, std::string_view key) const;

    // SHGetKnownFolderPath の結果をプロセス全体で 1 回だけ resolve する。
    // 関数ローカル static (magic static) は C++11 以降 thread-safe な lazy init を保証するため、
    // 別スレッド (並列計測 worker 等) から GetConfigDir() を呼んでも安全。
    static const std::filesystem::path& DefaultConfigDir();

    std::filesystem::path config_dir_override_;
    ini::IniData data_;
};

// セッション状態（最後に開いたファイル、ペイン構成、スクロール位置）の永続化を担当する。
// ConfigService のセクション "Session" / "Pane" を排他的に扱う薄いラッパ。
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
        float file_width = 220.0f;
        float toc_width = 220.0f;
    };
    void SavePaneState(const PaneState& state);
    // 狭いウィンドウで既定値が dynamic_max を超える対策。
    PaneState LoadPaneState(float client_width, float min_width, float default_width) const;

    struct ScrollPosition {
        int node = -1;
        int offset = 0;
    };
    void SaveScrollPosition(int node, float scroll_y, float node_y);
    ScrollPosition LoadScrollPosition() const;

private:
    ConfigService& config_;
};
