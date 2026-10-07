#pragma once
#include "resource.h"
#include <windows.h>
#include <atomic>
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
    std::wstring_view tooltip_pane_reveal;

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

    // ダイアグラムコピーボタン tooltip
    std::wstring_view tooltip_copy_diagram;

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
    std::wstring_view toast_image_save_failed;
    std::wstring_view toast_diagram_copying;
    std::wstring_view toast_diagram_copied;
    std::wstring_view toast_diagram_copy_failed;

    // ローディング
    std::wstring_view loading;

    // ダイアグラム描画エラー (詳細メッセージが取れなかった場合のフォールバック)
    std::wstring_view diagram_error;

    // ヘルプリソースID
    UINT help_resource_id;
};

inline constexpr Strings kJa = {
    .tooltip_open_file = L"ファイルを開く (Ctrl+O)",
    .tooltip_help = L"ヘルプ (F1)",
    .tooltip_theme_toggle = L"ダーク/ライトモード切替",
    .tooltip_search = L"検索 (Ctrl+F)",
    .tooltip_file_pane = L"ファイルペイン (Ctrl+1)",
    .tooltip_toc_pane = L"目次ペイン (Ctrl+2)",
    .tooltip_minimize = L"最小化",
    .tooltip_maximize = L"最大化",
    .tooltip_restore = L"元に戻す",
    .tooltip_close = L"閉じる",
    .tooltip_pane_close = L"閉じる",
    .tooltip_pane_refresh = L"更新",
    .tooltip_pane_reveal = L"現在のファイルの場所へ移動",
    .tooltip_search_prev = L"前のマッチ (Shift+Enter)",
    .tooltip_search_next = L"次のマッチ (Enter)",
    .tooltip_search_case = L"大文字/小文字を区別",
    .tooltip_search_highlight = L"全マッチをハイライト",
    .tooltip_search_close = L"閉じる (Esc)",
    .tooltip_nav_back = L"戻る (Alt+\u2190)",
    .tooltip_nav_forward = L"進む (Alt+\u2192)",
    .tooltip_copy = L"書式付きコピー",
    .tooltip_save_image = L"画像を保存",
    .tooltip_copy_diagram = L"クリップボードにコピー",
    .menu_edit_file = L"エディタで開く",
    .menu_copy = L"コピー",
    .menu_copy_formatted = L"書式付きコピー",
    .menu_dark_mode = L"ダークモード",
    .menu_file_pane = L"ファイルペイン",
    .menu_toc_pane = L"目次ペイン",
    .pane_header_files = L"ファイル",
    .pane_header_toc = L"目次",
    .menu_reset_window = L"ウィンドウ位置をリセット(&R)",
    .toast_file_not_found = L"ファイルが見つかりません",
    .toast_file_too_large = L"ファイルが大きすぎます",
    .toast_file_read_failed = L"ファイルの読み込みに失敗しました",
    .toast_image_saved = L"画像を保存しました",
    .toast_image_save_failed = L"画像の保存に失敗しました",
    .toast_diagram_copying = L"クリップボードにコピー中...",
    .toast_diagram_copied = L"クリップボードにコピーしました",
    .toast_diagram_copy_failed = L"コピーに失敗しました",
    .loading = L"読み込み中...",
    .diagram_error = L"図の描画に失敗しました",
    .help_resource_id = IDR_HELP_MD,
};

inline constexpr Strings kEn = {
    .tooltip_open_file = L"Open File (Ctrl+O)",
    .tooltip_help = L"Help (F1)",
    .tooltip_theme_toggle = L"Toggle Dark/Light Mode",
    .tooltip_search = L"Search (Ctrl+F)",
    .tooltip_file_pane = L"File Pane (Ctrl+1)",
    .tooltip_toc_pane = L"TOC Pane (Ctrl+2)",
    .tooltip_minimize = L"Minimize",
    .tooltip_maximize = L"Maximize",
    .tooltip_restore = L"Restore",
    .tooltip_close = L"Close",
    .tooltip_pane_close = L"Close",
    .tooltip_pane_refresh = L"Refresh",
    .tooltip_pane_reveal = L"Go to current file location",
    .tooltip_search_prev = L"Previous Match (Shift+Enter)",
    .tooltip_search_next = L"Next Match (Enter)",
    .tooltip_search_case = L"Match Case",
    .tooltip_search_highlight = L"Highlight All",
    .tooltip_search_close = L"Close (Esc)",
    .tooltip_nav_back = L"Back (Alt+\u2190)",
    .tooltip_nav_forward = L"Forward (Alt+\u2192)",
    .tooltip_copy = L"Copy as HTML",
    .tooltip_save_image = L"Save Image",
    .tooltip_copy_diagram = L"Copy to clipboard",
    .menu_edit_file = L"Open in Editor",
    .menu_copy = L"Copy",
    .menu_copy_formatted = L"Copy as HTML",
    .menu_dark_mode = L"Dark Mode",
    .menu_file_pane = L"File Pane",
    .menu_toc_pane = L"TOC Pane",
    .pane_header_files = L"Files",
    .pane_header_toc = L"TOC",
    .menu_reset_window = L"Reset Window Position (&R)",
    .toast_file_not_found = L"File not found",
    .toast_file_too_large = L"File is too large",
    .toast_file_read_failed = L"Failed to read file",
    .toast_image_saved = L"Image saved",
    .toast_image_save_failed = L"Failed to save image",
    .toast_diagram_copying = L"Copying to clipboard...",
    .toast_diagram_copied = L"Copied to clipboard",
    .toast_diagram_copy_failed = L"Copy failed",
    .loading = L"Loading...",
    .diagram_error = L"Failed to render diagram",
    .help_resource_id = IDR_HELP_EN_MD,
};

// std::atomic<const Strings*> にしてあるのは、テスト並列実行時 (ja/en を別スレッドで切り替える
// 言語切替テストなど) の data race を避けるため。kJa/kEn は static const なので
// load 値は常に有効ポインタで、参照剥がしも安全。
inline std::atomic<const Strings*> g_strings{ &kJa };

inline void Init(std::wstring_view config_lang) noexcept
{
    const Strings* selected = nullptr;
    if (config_lang == L"en") {
        selected = &kEn;
    }
    else if (config_lang == L"ja") {
        selected = &kJa;
    }
    else {
        const LANGID langid = GetUserDefaultUILanguage();
        selected = (PRIMARYLANGID(langid) == LANG_JAPANESE) ? &kJa : &kEn;
    }
    g_strings.store(selected, std::memory_order_release);
}

inline const Strings& S() noexcept
{
    return *g_strings.load(std::memory_order_acquire);
}

inline std::wstring_view GetLangKey() noexcept
{
    return (g_strings.load(std::memory_order_relaxed) == &kEn) ? L"en" : L"ja";
}

} // namespace i18n
