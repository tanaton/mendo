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
    // 右クリックメニューのウィンドウ名 (読み上げ用)。Windows の表記に合わせる。
    std::wstring_view menu_title;

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

    // 設定ダイアログ
    std::wstring_view settings_title;
    std::wstring_view settings_language;
    std::wstring_view settings_language_system;
    std::wstring_view settings_diagram_cache;
    std::wstring_view settings_clear_cache;

    // ヘルプリソースID
    UINT help_resource_id;

    // 設定ファイルの General/Language に保存する値。
    std::wstring_view lang_key;
    // 言語の一覧に出す名前。どの言語の表示中でもその言語自身の表記で出す。
    std::wstring_view lang_name;
    // この言語の文言 (UI の決まった文言と lang_name) を組むロケール。漢字の字形をこの言語に合わせる。
    const wchar_t* lang_locale;
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
    .menu_title = L"コンテキスト メニュー",
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
    .settings_title = L"設定",
    .settings_language = L"言語",
    .settings_language_system = L"OS の設定に合わせる",
    .settings_diagram_cache = L"図のキャッシュ",
    .settings_clear_cache = L"削除",
    .help_resource_id = IDR_HELP_MD,
    .lang_key = L"ja",
    .lang_name = L"日本語",
    .lang_locale = L"ja-jp",
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
    .menu_title = L"Context menu",
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
    .settings_title = L"Settings",
    .settings_language = L"Language",
    .settings_language_system = L"Use system setting",
    .settings_diagram_cache = L"Diagram cache",
    .settings_clear_cache = L"Clear",
    .help_resource_id = IDR_HELP_EN_MD,
    .lang_key = L"en",
    .lang_name = L"English",
    .lang_locale = L"en-us",
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
    .menu_title = L"上下文菜单",
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
    .settings_title = L"设置",
    .settings_language = L"语言",
    .settings_language_system = L"跟随系统设置",
    .settings_diagram_cache = L"图表缓存",
    .settings_clear_cache = L"清除",
    .help_resource_id = IDR_HELP_ZH_MD,
    .lang_key = L"zh",
    .lang_name = L"简体中文",
    .lang_locale = L"zh-cn",
};

inline constexpr Strings kZhHant = {
    .tooltip_open_file = L"開啟檔案 (Ctrl+O)",
    .tooltip_help = L"說明 (F1)",
    .tooltip_theme_toggle = L"切換深色/淺色模式",
    .tooltip_search = L"搜尋 (Ctrl+F)",
    .tooltip_file_pane = L"檔案窗格 (Ctrl+1)",
    .tooltip_toc_pane = L"目錄窗格 (Ctrl+2)",
    .tooltip_minimize = L"最小化",
    .tooltip_maximize = L"最大化",
    .tooltip_restore = L"還原",
    .tooltip_close = L"關閉",
    .tooltip_pane_close = L"關閉",
    .tooltip_pane_refresh = L"重新整理",
    .tooltip_pane_reveal = L"前往目前檔案所在位置",
    .tooltip_search_prev = L"上一個相符項目 (Shift+Enter)",
    .tooltip_search_next = L"下一個相符項目 (Enter)",
    .tooltip_search_case = L"大小寫須相符",
    .tooltip_search_highlight = L"醒目提示所有相符項目",
    .tooltip_search_close = L"關閉 (Esc)",
    .tooltip_nav_back = L"上一頁 (Alt+←)",
    .tooltip_nav_forward = L"下一頁 (Alt+→)",
    .gesture_back = L"← 上一頁",
    .gesture_forward = L"→ 下一頁",
    .tooltip_copy = L"複製為 HTML",
    .tooltip_save_image = L"儲存圖片",
    .tooltip_copy_diagram = L"複製到剪貼簿",
    .menu_edit_file = L"在編輯器中開啟",
    .menu_copy = L"複製",
    .menu_copy_formatted = L"複製為 HTML",
    .menu_dark_mode = L"深色模式",
    .menu_file_pane = L"檔案窗格",
    .menu_toc_pane = L"目錄窗格",
    .menu_zoom = L"縮放",
    .menu_title = L"內容功能表",
    .pane_header_files = L"檔案",
    .pane_header_toc = L"目錄",
    .menu_reset_window = L"重設視窗位置(&R)",
    .toast_file_not_found = L"找不到檔案",
    .toast_file_too_large = L"檔案太大",
    .toast_file_read_failed = L"無法讀取檔案",
    .toast_image_saved = L"已儲存圖片",
    .toast_image_save_failed = L"無法儲存圖片",
    .toast_diagram_copying = L"正在複製到剪貼簿...",
    .toast_diagram_copied = L"已複製到剪貼簿",
    .toast_diagram_copy_failed = L"複製失敗",
    .loading = L"正在載入...",
    .diagram_error = L"無法繪製圖表",
    .diagram_too_large = L"圖表太大，無法顯示",
    .settings_title = L"設定",
    .settings_language = L"語言",
    .settings_language_system = L"依照系統設定",
    .settings_diagram_cache = L"圖表快取",
    .settings_clear_cache = L"清除",
    .help_resource_id = IDR_HELP_ZH_HANT_MD,
    .lang_key = L"zh-hant",
    .lang_name = L"繁體中文",
    .lang_locale = L"zh-tw",
};

inline constexpr Strings kKo = {
    .tooltip_open_file = L"파일 열기 (Ctrl+O)",
    .tooltip_help = L"도움말 (F1)",
    .tooltip_theme_toggle = L"다크/라이트 모드 전환",
    .tooltip_search = L"검색 (Ctrl+F)",
    .tooltip_file_pane = L"파일 창 (Ctrl+1)",
    .tooltip_toc_pane = L"목차 창 (Ctrl+2)",
    .tooltip_minimize = L"최소화",
    .tooltip_maximize = L"최대화",
    .tooltip_restore = L"이전 크기로 복원",
    .tooltip_close = L"닫기",
    .tooltip_pane_close = L"닫기",
    .tooltip_pane_refresh = L"새로 고침",
    .tooltip_pane_reveal = L"현재 파일 위치로 이동",
    .tooltip_search_prev = L"이전 일치 항목 (Shift+Enter)",
    .tooltip_search_next = L"다음 일치 항목 (Enter)",
    .tooltip_search_case = L"대/소문자 구분",
    .tooltip_search_highlight = L"모든 일치 항목 강조 표시",
    .tooltip_search_close = L"닫기 (Esc)",
    .tooltip_nav_back = L"뒤로 (Alt+←)",
    .tooltip_nav_forward = L"앞으로 (Alt+→)",
    .gesture_back = L"← 뒤로",
    .gesture_forward = L"→ 앞으로",
    .tooltip_copy = L"HTML로 복사",
    .tooltip_save_image = L"이미지 저장",
    .tooltip_copy_diagram = L"클립보드에 복사",
    .menu_edit_file = L"편집기에서 열기",
    .menu_copy = L"복사",
    .menu_copy_formatted = L"HTML로 복사",
    .menu_dark_mode = L"다크 모드",
    .menu_file_pane = L"파일 창",
    .menu_toc_pane = L"목차 창",
    .menu_zoom = L"확대/축소",
    .menu_title = L"상황에 맞는 메뉴",
    .pane_header_files = L"파일",
    .pane_header_toc = L"목차",
    .menu_reset_window = L"창 위치 초기화(&R)",
    .toast_file_not_found = L"파일을 찾을 수 없습니다",
    .toast_file_too_large = L"파일이 너무 큽니다",
    .toast_file_read_failed = L"파일을 읽지 못했습니다",
    .toast_image_saved = L"이미지를 저장했습니다",
    .toast_image_save_failed = L"이미지를 저장하지 못했습니다",
    .toast_diagram_copying = L"클립보드에 복사하는 중...",
    .toast_diagram_copied = L"클립보드에 복사했습니다",
    .toast_diagram_copy_failed = L"복사하지 못했습니다",
    .loading = L"불러오는 중...",
    .diagram_error = L"다이어그램을 그리지 못했습니다",
    .diagram_too_large = L"다이어그램이 너무 커서 표시할 수 없습니다",
    .settings_title = L"설정",
    .settings_language = L"언어",
    .settings_language_system = L"시스템 설정 사용",
    .settings_diagram_cache = L"다이어그램 캐시",
    .settings_clear_cache = L"삭제",
    .help_resource_id = IDR_HELP_KO_MD,
    .lang_key = L"ko",
    .lang_name = L"한국어",
    .lang_locale = L"ko-kr",
};

// 書き換えは UI スレッドだけだが、読む側のスレッドを縛らないよう std::atomic<const Strings*> にする。
// 各言語の表は static const なので load 値は常に有効ポインタで、参照剥がしも安全。
inline std::atomic<const Strings*> g_strings{ &kJa };

// 台湾・香港・マカオと、地域なしの zh-Hant を繁体字とみなす。残りの中国語 (中国本土・シンガポール・zh-Hans) は簡体字。
inline bool IsTraditionalChinese(LANGID langid) noexcept
{
    if (PRIMARYLANGID(langid) != LANG_CHINESE) {
        return false;
    }
    const WORD sub = SUBLANGID(langid);
    return sub == SUBLANG_CHINESE_TRADITIONAL || sub == SUBLANG_CHINESE_HONGKONG || sub == SUBLANG_CHINESE_MACAU ||
           langid == LANG_CHINESE_TRADITIONAL;
}

// 設定に言語が無いとき、OS の表示言語から選ぶ文言。
inline const Strings* StringsForUiLanguage(LANGID langid) noexcept
{
    switch (PRIMARYLANGID(langid)) {
    case LANG_JAPANESE:
        return &kJa;
    case LANG_KOREAN:
        return &kKo;
    case LANG_CHINESE:
        return IsTraditionalChinese(langid) ? &kZhHant : &kZh;
    default:
        return &kEn;
    }
}

// OS の表示言語に合わせたときの文言。
inline const Strings& SystemStrings() noexcept
{
    return *StringsForUiLanguage(GetUserDefaultUILanguage());
}

// 設定ダイアログの一覧の並び。
inline constexpr const Strings* kAllStrings[] = { &kJa, &kEn, &kZh, &kZhHant, &kKo };

// 設定で選んだ言語。nullptr は OS の表示言語に合わせる。g_strings と同じ理由で atomic にする。
inline std::atomic<const Strings*> g_chosen{ nullptr };

// chosen が nullptr なら OS の表示言語に合わせる。表示中の文言はすぐ切り替わる (文言は使うたびに S() から引く)。
// 表示する文言が変わったら true (「OS に合わせる」と OS と同じ言語の間では変わらない)。
inline bool SetLanguage(const Strings* chosen) noexcept
{
    g_chosen.store(chosen, std::memory_order_relaxed);
    const Strings* const shown = chosen ? chosen : &SystemStrings();
    return g_strings.exchange(shown, std::memory_order_acq_rel) != shown;
}

// config_lang は設定ファイルの値。空や知らない値は OS に合わせる。
inline void Init(std::wstring_view config_lang) noexcept
{
    const auto it = std::ranges::find(kAllStrings, config_lang, &Strings::lang_key);
    SetLanguage(it != std::end(kAllStrings) ? *it : nullptr);
}

inline const Strings* ChosenLanguage() noexcept
{
    return g_chosen.load(std::memory_order_relaxed);
}

inline const Strings& S() noexcept
{
    return *g_strings.load(std::memory_order_acquire);
}

// 設定ファイルに保存する値。OS に合わせるときは空にし、次回も OS の表示言語で決める。
inline std::wstring_view ConfigLangKey() noexcept
{
    const Strings* chosen = ChosenLanguage();
    return chosen ? chosen->lang_key : std::wstring_view{};
}

// 本文の書式に渡すロケール。漢字は同じ符号でも地域で字形が違い、DirectWrite はこれでフォールバック先を選ぶ
// (ja-jp: Yu Gothic UI、zh-cn: Microsoft YaHei UI、zh-tw / zh-hk: Microsoft JhengHei UI、ko-kr: Malgun Gothic)。
// 文書の言語は UI の言語設定 (英語 UI で日本語を読む人もいる) ではなく OS の表示言語から推す。
// 確定仕様: これを使うのは文書から来る文字 (本文・ファイル名・目次・タイトル・検索語) だけ。メニュー・ペインの見出し・
// トースト・設定ダイアログなど UI の決まった文言は、選んだ言語の Strings::lang_locale で組む。OS と違う言語を選んでも
// UI の漢字をその言語の字形で出し、UI のどこでも字形を揃えるため。
// 漢字を使わない英語 (と文言を持たない言語) は従来どおり日本語の字形にする。
inline const wchar_t* TextLocaleForUiLanguage(LANGID langid) noexcept
{
    const Strings* strings = StringsForUiLanguage(langid);
    if (strings == &kEn) {
        return kJa.lang_locale;
    }
    // 香港・マカオは同じ繁体字でも台湾と字形が違う。
    const WORD sub = SUBLANGID(langid);
    if (strings == &kZhHant && (sub == SUBLANG_CHINESE_HONGKONG || sub == SUBLANG_CHINESE_MACAU)) {
        return L"zh-hk";
    }
    return strings->lang_locale;
}

// Theme::text_locale がこれを持ち、本文と同じ書体の書式に渡す。
inline const wchar_t* TextLocale() noexcept
{
    static const wchar_t* const locale = TextLocaleForUiLanguage(GetUserDefaultUILanguage());
    return locale;
}

} // namespace i18n
