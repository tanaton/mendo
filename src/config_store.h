#pragma once
#include <string>
#include <filesystem>

namespace config {

// 設定ディレクトリを上書きする（テスト用）。デフォルトに戻すには空のパスを渡す。
void SetConfigDirOverride(const std::filesystem::path& dir);

// mendoの設定ディレクトリを返す（%LOCALAPPDATA%/mendo）。
std::filesystem::path GetConfigDir();

// 設定ディレクトリ内の設定ファイルのフルパスを返す。
std::filesystem::path GetConfigPath(const wchar_t* filename);

// 真偽値を保存/読み込みする（'0' または '1' として保存）。
void SaveBool(const wchar_t* filename, bool value);
bool LoadBool(const wchar_t* filename, bool default_value = false);

// 範囲チェック付きで整数値を保存/読み込みする。
void SaveInt(const wchar_t* filename, int value);
int LoadInt(const wchar_t* filename, int default_value, int min_val, int max_val);

// ワイド文字列を保存/読み込みする（UTF-16LEバイナリとして保存）。
void SaveWString(const wchar_t* filename, std::wstring_view value);
std::pmr::wstring LoadWString(const wchar_t* filename);

} // namespace config
