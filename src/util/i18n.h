#pragma once
#include "resource.h"
#include <windows.h>
#include <algorithm>
#include <atomic>
#include <iterator>
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

    // ジェスチャー/スワイプのオーバーレイ
    std::wstring_view gesture_back;
    std::wstring_view gesture_forward;

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
    std::wstring_view menu_zoom;

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
    std::wstring_view diagram_too_large;

    // ヘルプリソースID
    UINT help_resource_id;

    // 設定ファイルの General/Language に保存する値。
    std::wstring_view lang_key;
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
    .gesture_back = L"\u2190 戻る",
    .gesture_forward = L"\u2192 進む",
    .tooltip_copy = L"書式付きコピー",
    .tooltip_save_image = L"画像を保存",
    .tooltip_copy_diagram = L"クリップボードにコピー",
    .menu_edit_file = L"エディタで開く",
    .menu_copy = L"コピー",
    .menu_copy_formatted = L"書式付きコピー",
    .menu_dark_mode = L"ダークモード",
    .menu_file_pane = L"ファイルペイン",
    .menu_toc_pane = L"目次ペイン",
    .menu_zoom = L"ズーム",
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
    .diagram_too_large = L"図が大きすぎるため表示できません",
    .help_resource_id = IDR_HELP_MD,
    .lang_key = L"ja",
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
    .gesture_back = L"\u2190 Back",
    .gesture_forward = L"\u2192 Forward",
    .tooltip_copy = L"Copy as HTML",
    .tooltip_save_image = L"Save Image",
    .tooltip_copy_diagram = L"Copy to clipboard",
    .menu_edit_file = L"Open in Editor",
    .menu_copy = L"Copy",
    .menu_copy_formatted = L"Copy as HTML",
    .menu_dark_mode = L"Dark Mode",
    .menu_file_pane = L"File Pane",
    .menu_toc_pane = L"TOC Pane",
    .menu_zoom = L"Zoom",
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
    .diagram_too_large = L"Diagram is too large to display",
    .help_resource_id = IDR_HELP_EN_MD,
    .lang_key = L"en",
};

inline constexpr Strings kZh = {
    .tooltip_open_file = L"打开文件 (Ctrl+O)",
    .tooltip_help = L"帮助 (F1)",
    .tooltip_theme_toggle = L"切换深色/浅色模式",
    .tooltip_search = L"搜索 (Ctrl+F)",
    .tooltip_file_pane = L"文件窗格 (Ctrl+1)",
    .tooltip_toc_pane = L"目录窗格 (Ctrl+2)",
    .tooltip_minimize = L"最小化",
    .tooltip_maximize = L"最大化",
    .tooltip_restore = L"还原",
    .tooltip_close = L"关闭",
    .tooltip_pane_close = L"关闭",
    .tooltip_pane_refresh = L"刷新",
    .tooltip_pane_reveal = L"转到当前文件所在位置",
    .tooltip_search_prev = L"上一个匹配 (Shift+Enter)",
    .tooltip_search_next = L"下一个匹配 (Enter)",
    .tooltip_search_case = L"区分大小写",
    .tooltip_search_highlight = L"突出显示所有匹配",
    .tooltip_search_close = L"关闭 (Esc)",
    .tooltip_nav_back = L"后退 (Alt+←)",
    .tooltip_nav_forward = L"前进 (Alt+→)",
    .gesture_back = L"← 后退",
    .gesture_forward = L"→ 前进",
    .tooltip_copy = L"复制为 HTML",
    .tooltip_save_image = L"保存图像",
    .tooltip_copy_diagram = L"复制到剪贴板",
    .menu_edit_file = L"在编辑器中打开",
    .menu_copy = L"复制",
    .menu_copy_formatted = L"复制为 HTML",
    .menu_dark_mode = L"深色模式",
    .menu_file_pane = L"文件窗格",
    .menu_toc_pane = L"目录窗格",
    .menu_zoom = L"缩放",
    .pane_header_files = L"文件",
    .pane_header_toc = L"目录",
    .menu_reset_window = L"重置窗口位置(&R)",
    .toast_file_not_found = L"找不到文件",
    .toast_file_too_large = L"文件过大",
    .toast_file_read_failed = L"读取文件失败",
    .toast_image_saved = L"已保存图像",
    .toast_image_save_failed = L"保存图像失败",
    .toast_diagram_copying = L"正在复制到剪贴板...",
    .toast_diagram_copied = L"已复制到剪贴板",
    .toast_diagram_copy_failed = L"复制失败",
    .loading = L"正在加载...",
    .diagram_error = L"图表渲染失败",
    .diagram_too_large = L"图表过大，无法显示",
    .help_resource_id = IDR_HELP_ZH_MD,
    .lang_key = L"zh",
};

// std::atomic<const Strings*> にしてあるのは、テスト並列実行時 (ja/en を別スレッドで切り替える
// 言語切替テストなど) の data race を避けるため。kJa/kEn/kZh は static const なので
// load 値は常に有効ポインタで、参照剥がしも安全。
inline std::atomic<const Strings*> g_strings{ &kJa };

// 繁体字の文言は持たないので、簡体字の地域だけ中国語にする。
inline bool IsSimplifiedChinese(LANGID langid) noexcept
{
    return PRIMARYLANGID(langid) == LANG_CHINESE &&
           (SUBLANGID(langid) == SUBLANG_CHINESE_SIMPLIFIED || SUBLANGID(langid) == SUBLANG_CHINESE_SINGAPORE);
}

inline constexpr const Strings* kAllStrings[] = { &kJa, &kEn, &kZh };

inline void Init(std::wstring_view config_lang) noexcept
{
    const auto it = std::ranges::find(kAllStrings, config_lang, &Strings::lang_key);
    const Strings* selected = nullptr;
    if (it != std::end(kAllStrings)) {
        selected = *it;
    }
    else {
        const LANGID langid = GetUserDefaultUILanguage();
        selected = PRIMARYLANGID(langid) == LANG_JAPANESE ? &kJa
                   : IsSimplifiedChinese(langid)          ? &kZh
                                                          : &kEn;
    }
    g_strings.store(selected, std::memory_order_release);
}

inline const Strings& S() noexcept
{
    return *g_strings.load(std::memory_order_acquire);
}

inline std::wstring_view GetLangKey() noexcept
{
    return g_strings.load(std::memory_order_relaxed)->lang_key;
}

// 本文の書式に渡すロケール。漢字は同じ符号でも地域で字形が違い、DirectWrite はこれでフォールバック先を選ぶ
// (ja-jp: Yu Gothic UI、zh-cn: Microsoft YaHei UI、zh-tw: Microsoft JhengHei UI)。
// 文書の言語は UI の言語設定 (英語 UI で日本語を読む人もいる) ではなく OS の表示言語から推す。
// Theme::text_locale がこれを持ち、本文と同じ書体の書式に渡す。
// 中国語以外は従来どおり日本語の字形にする。
inline const wchar_t* TextLocale() noexcept
{
    static const wchar_t* const locale = [] {
        const LANGID langid = GetUserDefaultUILanguage();
        if (PRIMARYLANGID(langid) != LANG_CHINESE) {
            return L"ja-jp";
        }
        if (IsSimplifiedChinese(langid)) {
            return L"zh-cn";
        }
        return SUBLANGID(langid) == SUBLANG_CHINESE_TRADITIONAL ? L"zh-tw" : L"zh-hk";
    }();
    return locale;
}

} // namespace i18n
