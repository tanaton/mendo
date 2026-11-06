#pragma once
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

enum class StartupArgKind : uint8_t {
    None,      // 引数なし
    File,      // 実在するファイル
    Directory, // 実在するフォルダ
    Invalid,   // 実在しないパス
};

// 起動時の引数。mendo.exe [パス] [--anchor 見出し]
struct StartupArgs {
    std::wstring_view path;
    std::wstring_view anchor;
};

// argv は exe の名前を除いた引数。--anchor の次の 1 つを見出しにし、ほかの最初の 1 つをパスにする (並びは問わない)。
// 見出しの先頭の # は外す。
StartupArgs ParseStartupArgs(std::span<const wchar_t* const> argv);
// 新しいウィンドウに渡す引数。ParseStartupArgs で path と anchor に戻る。path が空なら引数なし。
std::vector<std::wstring_view> BuildStartupArgs(std::wstring_view path, std::wstring_view anchor);

// 起動時に OS から集めた情報。パスはすべて絶対パスで渡す。
struct StartupContext {
    StartupArgKind arg_kind = StartupArgKind::None;
    std::wstring_view arg_path;
    // --anchor の見出し。ファイルを指定したときだけ使う。
    std::wstring_view anchor;
    // session::LoadLastFilePath 済み。空なら復元対象なし。
    std::wstring_view last_file;
    std::wstring_view cwd;
    // システムフォルダ・exe のフォルダなど、ユーザーの作業場所とみなさないフォルダ。
    std::span<const std::wstring_view> ignored_cwds;
};

struct StartupPlan {
    // 空ならヘルプを表示する。
    std::wstring document_path;
    // 空ならファイルペインを設定しない。
    std::wstring pane_directory;
    // 空でなければ、開いた文書でこの見出しへ飛ぶ。前回のスクロール位置は戻さない。
    std::wstring anchor;
    bool restore_scroll = false;
    // 引数のファイルが前回のファイルと表記違いの同一ファイルかもしれない (スラッシュ違いなど)。同じなら復元する。
    // 確かめるには 2 ファイルを開くので、呼び出し側がプリロードを始めてから調べる。
    bool restore_scroll_if_same_file = false;
};

StartupPlan DecideStartupPlan(const StartupContext& ctx);
