#pragma once
#include <memory_resource>
#include <windows.h>

// ファイル選択ダイアログを表示するユーティリティ。
// `<commdlg.h>` を call site に巻き込まないようヘッダで宣言だけを露出する。
// ファイル読み込み (FileLoader::LoadFile) と分離することで「path を受け取って読む」
// 責務と「path を選ばせる」責務を分け、テストではダイアログ呼出をスキップして
// 任意の path を直接渡せるようにする。
namespace file_dialog_service {

// キャンセル時は空文字列。
[[nodiscard]] std::pmr::wstring OpenMarkdownFileDialog(HWND owner);

// キャンセル時は空文字列。
[[nodiscard]] std::pmr::wstring SavePngFileDialog(HWND owner, const wchar_t* default_filename);

} // namespace file_dialog_service
