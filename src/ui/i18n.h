#pragma once
#include "resource.h"
#include <windows.h>
#include <string_view>

namespace i18n {

struct Strings {
    // タイトルバー tooltip
    std::wstring_view tooltip_open_file;
    std::wstring_view tooltip_help;
    std::wstring_view tooltip_theme_toggle;
    std::wstring_view tooltip_search;
    std::wstring_view tooltip_file_pane;
    std::wstring_view tooltip_toc_pane;
    std::wstring_view tooltip_minimize;
    std::wstring_view tooltip_maximize;
    std::wstring_view tooltip_restore;
    std::wstring_view tooltip_close;

    // ペインボタン tooltip
    std::wstring_view tooltip_pane_close;
    std::wstring_view tooltip_pane_refresh;

    // 検索バー tooltip
    std::wstring_view tooltip_search_prev;
    std::wstring_view tooltip_search_next;
    std::wstring_view tooltip_search_case;
    std::wstring_view tooltip_search_highlight;
    std::wstring_view tooltip_search_close;

    // ナビゲーション tooltip
    std::wstring_view tooltip_nav_back;
    std::wstring_view tooltip_nav_forward;

    // コピーボタン tooltip
    std::wstring_view tooltip_copy;

    // 保存ボタン tooltip
    std::wstring_view tooltip_save_image;

    // SVG コピーボタン tooltip
    std::wstring_view tooltip_copy_svg;

    // コンテキストメニュー
    std::wstring_view menu_edit_file;
    std::wstring_view menu_copy;
    std::wstring_view menu_copy_formatted;
    std::wstring_view menu_dark_mode;
    std::wstring_view menu_file_pane;
    std::wstring_view menu_toc_pane;

    // ペインヘッダー
    std::wstring_view pane_header_files;
    std::wstring_view pane_header_toc;

    // システムメニュー
    std::wstring_view menu_reset_window;

    // トースト
    std::wstring_view toast_file_not_found;
    std::wstring_view toast_file_too_large;
    std::wstring_view toast_file_read_failed;
    std::wstring_view toast_image_saved;
    std::wstring_view toast_svg_copying;
    std::wstring_view toast_svg_copied;
    std::wstring_view toast_svg_copy_failed;

    // ローディング
    std::wstring_view loading;

    // ヘルプリソースID
    UINT help_resource_id;
};

inline constexpr Strings kJa = {
    // タイトルバー tooltip
    L"ファイルを開く (Ctrl+O)",
    L"ヘルプ (F1)",
    L"ダーク/ライトモード切替",
    L"検索 (Ctrl+F)",
    L"ファイルペイン (Ctrl+1)",
    L"目次ペイン (Ctrl+2)",
    L"最小化",
    L"最大化",
    L"元に戻す",
    L"閉じる",
    // ペインボタン tooltip
    L"閉じる",
    L"更新",
    // 検索バー tooltip
    L"前のマッチ (Shift+Enter)",
    L"次のマッチ (Enter)",
    L"大文字/小文字を区別",
    L"全マッチをハイライト",
    L"閉じる (Esc)",
    // ナビゲーション tooltip
    L"戻る (Alt+\u2190)",
    L"進む (Alt+\u2192)",
    // コピーボタン tooltip
    L"コピー",
    // 保存ボタン tooltip
    L"画像を保存",
    // SVG コピーボタン tooltip
    L"SVGとしてコピー",
    // コンテキストメニュー
    L"エディタで開く",
    L"コピー",
    L"書式付きコピー",
    L"ダークモード",
    L"ファイルペイン",
    L"目次ペイン",
    // ペインヘッダー
    L"ファイル",
    L"目次",
    // システムメニュー
    L"ウィンドウ位置をリセット(&R)",
    // トースト
    L"ファイルが見つかりません",
    L"ファイルが大きすぎます",
    L"ファイルの読み込みに失敗しました",
    L"画像を保存しました",
    L"SVGをコピー中...",
    L"SVGをコピーしました",
    L"SVGのコピーに失敗しました",
    // ローディング
    L"読み込み中...",
    // ヘルプリソースID
    IDR_HELP_MD,
};

inline constexpr Strings kEn = {
    // タイトルバー tooltip
    L"Open File (Ctrl+O)",
    L"Help (F1)",
    L"Toggle Dark/Light Mode",
    L"Search (Ctrl+F)",
    L"File Pane (Ctrl+1)",
    L"TOC Pane (Ctrl+2)",
    L"Minimize",
    L"Maximize",
    L"Restore",
    L"Close",
    // ペインボタン tooltip
    L"Close",
    L"Refresh",
    // 検索バー tooltip
    L"Previous Match (Shift+Enter)",
    L"Next Match (Enter)",
    L"Match Case",
    L"Highlight All",
    L"Close (Esc)",
    // ナビゲーション tooltip
    L"Back (Alt+\u2190)",
    L"Forward (Alt+\u2192)",
    // コピーボタン tooltip
    L"Copy",
    // 保存ボタン tooltip
    L"Save Image",
    // SVG コピーボタン tooltip
    L"Copy as SVG",
    // コンテキストメニュー
    L"Open in Editor",
    L"Copy",
    L"Copy as HTML",
    L"Dark Mode",
    L"File Pane",
    L"TOC Pane",
    // ペインヘッダー
    L"Files",
    L"TOC",
    // システムメニュー
    L"Reset Window Position (&R)",
    // トースト
    L"File not found",
    L"File is too large",
    L"Failed to read file",
    L"Image saved",
    L"Copying SVG...",
    L"SVG copied",
    L"Failed to copy SVG",
    // ローディング
    L"Loading...",
    // ヘルプリソースID
    IDR_HELP_EN_MD,
};

inline const Strings* g_strings = &kJa;

// 起動時に1回呼び出す。config_lang が "ja"/"en" なら直接選択、
// 空または未知の場合は OS の UI 言語から自動判定する。
inline void Init(std::wstring_view config_lang) noexcept
{
    if (config_lang == L"en") {
        g_strings = &kEn;
    }
    else if (config_lang == L"ja") {
        g_strings = &kJa;
    }
    else {
        const LANGID langid = GetUserDefaultUILanguage();
        g_strings = (PRIMARYLANGID(langid) == LANG_JAPANESE) ? &kJa : &kEn;
    }
}

inline const Strings& S() noexcept { return *g_strings; }

// 現在の言語を設定ファイル用のキー文字列で返す。
inline std::wstring_view GetLangKey() noexcept
{
    return (g_strings == &kEn) ? L"en" : L"ja";
}

} // namespace i18n
