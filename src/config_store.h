#pragma once
#include <string>
#include <string_view>
#include <filesystem>
#include <memory_resource>

namespace config {

// 設定ディレクトリを上書きする（テスト用）。デフォルトに戻すには空のパスを渡す。
void SetConfigDirOverride(const std::filesystem::path& dir);

// mendoの設定ディレクトリを返す（%LOCALAPPDATA%/mendo）。
std::filesystem::path GetConfigDir();

// 設定ディレクトリ内のファイルのフルパスを返す（MermaidFileCache等で使用）。
std::filesystem::path GetConfigPath(std::wstring_view filename);

// INIファイルをディスクからメモリに読み込む。起動時に1回呼ぶ。
void Load();

// メモリ上のデータをディスクに書き込む。終了時や即時保存時に呼ぶ。
void Save();

// メモリ上のデータをクリアする（テスト用）。
void Clear();

// ---- 型付きアクセサ（メモリ上のマップを読み書き） ----

void SetBool(std::string_view section, std::string_view key, bool value);
bool GetBool(std::string_view section, std::string_view key, bool default_value = false);

void SetInt(std::string_view section, std::string_view key, int value);
int GetInt(std::string_view section, std::string_view key, int default_value, int min_val, int max_val);

void SetWString(std::string_view section, std::string_view key, std::wstring_view value);
std::pmr::wstring GetWString(std::string_view section, std::string_view key);

} // namespace config
