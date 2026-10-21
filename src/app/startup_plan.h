#pragma once
#include <cstdint>
#include <memory_resource>
#include <span>
#include <string>
#include <string_view>

enum class StartupArgKind : uint8_t {
    None,      // 引数なし
    File,      // 実在するファイル
    Directory, // 実在するフォルダ
    Invalid,   // 実在しないパス
};

// 起動時に OS から集めた情報。パスはすべて絶対パスで渡す。
struct StartupContext {
    StartupArgKind arg_kind = StartupArgKind::None;
    std::wstring_view arg_path;
    // arg_path が前回終了時のファイルと同じ表記か (大文字小文字は区別しない。File のときのみ意味を持つ)。
    bool arg_is_last_file = false;
    // SessionService::LoadLastFilePath 済み。空なら復元対象なし。
    std::wstring_view last_file;
    std::wstring_view cwd;
    // システムフォルダ・exe のフォルダなど、ユーザーの作業場所とみなさないフォルダ。
    std::span<const std::wstring_view> ignored_cwds;
};

struct StartupPlan {
    // 空ならヘルプを表示する。
    std::pmr::wstring document_path;
    // 空ならファイルペインを設定しない。
    std::pmr::wstring pane_directory;
    bool restore_scroll = false;
    // 引数のファイルが前回のファイルと表記違いの同一ファイルかもしれない (スラッシュ違いなど)。同じなら復元する。
    // 確かめるには 2 ファイルを開くので、呼び出し側がプリロードを始めてから調べる。
    bool restore_scroll_if_same_file = false;
};

StartupPlan DecideStartupPlan(const StartupContext& ctx);
