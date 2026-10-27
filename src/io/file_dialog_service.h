#pragma once
#include <string>
#include <windows.h>

// ファイル選択ダイアログ。`<commdlg.h>` を call site に巻き込まないよう宣言だけを露出する。
namespace file_dialog_service {

// キャンセル時は空文字列。
[[nodiscard]] std::wstring OpenMarkdownFileDialog(HWND owner);

// キャンセル時は空文字列。
[[nodiscard]] std::wstring SavePngFileDialog(HWND owner, const wchar_t* default_filename);

} // namespace file_dialog_service
