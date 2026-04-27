#pragma once
#include "ini_parser.h"
#include <string>
#include <string_view>
#include <filesystem>
#include <memory_resource>

class PaneController;

// 設定 (settings.ini) のメモリ上マップとディスク永続化を保持する。
// インスタンスごとに独立した状態を持つので、テストでは新しいインスタンスを使えば良い。
class ConfigService {
public:
    ConfigService() noexcept = default;

    // ---- 設定ディレクトリ ----

    // テスト用に既定の AppData パスをオーバーライドする。
    void SetConfigDirOverride(const std::filesystem::path& dir);
    std::filesystem::path GetConfigDir() const;
    // 設定ディレクトリ配下のファイルパスを返す。区切り文字や ".." を含む名前は拒否する。
    std::filesystem::path GetConfigPath(std::wstring_view filename) const;

    // ---- ディスク永続化 ----

    void Load();
    // メモリ上のデータをディスクに書き出す。書き込みはアプリ終了時 (WM_DESTROY) に集約する。
    void Flush();
    void Clear() noexcept;

    // ---- 型付きアクセサ（メモリ上のマップを読み書き） ----

    void SaveBool(std::string_view section, std::string_view key, bool value);
    bool LoadBool(std::string_view section, std::string_view key, bool default_value = false) const;

    void SaveInt(std::string_view section, std::string_view key, int value);
    int LoadInt(std::string_view section, std::string_view key, int def, int min_v, int max_v) const;

    void SaveWString(std::string_view section, std::string_view key, std::wstring_view value);
    std::pmr::wstring LoadWString(std::string_view section, std::string_view key) const;

private:
    [[nodiscard]] const std::string* FindValue(std::string_view section, std::string_view key) const;

    std::filesystem::path config_dir_override_;
    // SHGetKnownFolderPath の結果キャッシュ。override 未設定時の問い合わせを 1 回に抑える。
    mutable std::filesystem::path cached_default_dir_;
    ini::IniData data_;
};

// セッション状態（最後に開いたファイル、ペイン構成、スクロール位置）の永続化を担当する。
// ConfigService のセクション "Session" / "Pane" を排他的に扱う薄いラッパ。
class SessionService {
public:
    explicit SessionService(ConfigService& config) noexcept : config_(config) {}

    void SaveLastFilePath(std::wstring_view path);
    std::pmr::wstring LoadLastFilePath() const;

    void SavePaneState(const PaneController& panes);
    void LoadPaneState(PaneController& panes, float client_width);

    struct ScrollPosition {
        int node = -1;
        int offset = 0;
    };
    void SaveScrollPosition(int node, float scroll_y, float node_y);
    ScrollPosition LoadScrollPosition() const;

private:
    ConfigService& config_;
};
