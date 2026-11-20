#pragma once
#include "i18n.h"
#include "popup_window.h"
#include "ui_types.h"
#include <array>
#include <functional>
#include <iterator>
#include <optional>
#include <string>
#include <string_view>
#include <windows.h>

struct SettingsDialogParams {
    PopupEnv env;
    // 言語を選んだ。nullptr は OS に合わせる。呼んだ後にダイアログ自身の文言も選んだ言語で組み直す。
    std::function<void(const i18n::Strings*)> on_language;
};

namespace settings_dialog_constants {
inline constexpr float MIN_WIDTH = 320.0f;
inline constexpr float PAD_X = 16.0f;
inline constexpr float TITLE_HEIGHT = 44.0f;
inline constexpr float TITLE_FONT_SCALE = 1.25f;
inline constexpr float HEADING_HEIGHT = 24.0f;
inline constexpr float ROW_HEIGHT = 28.0f;
inline constexpr float CHECK_WIDTH = 24.0f;
inline constexpr float HOVER_MARGIN_X = 8.0f;
inline constexpr float BOTTOM_PAD = 10.0f;
inline constexpr float ICON_FONT_SIZE = 12.0f;

// 言語の一覧の行。先頭は OS に合わせる (nullptr)、残りは i18n::kAllStrings の並び。
inline constexpr size_t kLanguageRowCount = std::size(i18n::kAllStrings) + 1;
} // namespace settings_dialog_constants

// 本体と同じ色で描くため自前描画。開くたびに作る。テストはウィンドウを作らずに各段階を直接呼ぶ。
struct SettingsDialog : PopupWindow<SettingsDialog> {
    static constexpr wchar_t kClassName[] = L"mendoSettingsDialog";

    // 閉じるまで戻らない (入力はダイアログが取る)。
    static void Show(HWND owner_hwnd, const SettingsDialogParams& params);

    // 大きさは PopupWindow::width / height。
    struct Layout {
        DipRect language_heading{};
        std::array<DipRect, settings_dialog_constants::kLanguageRowCount> languages{};
        // 言語の一覧とバージョンの間の区切り線の行。
        DipRect separator{};
        DipRect version{};
    };

    // 一番下に出す。タイトルバーはタスクバーや Alt+Tab にも出てファイル名の場所を減らすので使わない (ユーザーが選んだ)。
    // 訳さずに済むよう、名前とリリースのタグの形 (mendo v0.5.1.0) にする。
    static std::wstring_view VersionText() noexcept;

    static constexpr const i18n::Strings* LanguageOfRow(size_t row) noexcept
    {
        return row == 0 ? nullptr : i18n::kAllStrings[row - 1];
    }
    struct RowLabel {
        std::wstring_view text;
        IDWriteTextFormat* fmt;
    };
    RowLabel LabelOfRow(size_t row) const noexcept
    {
        if (const i18n::Strings* language = LanguageOfRow(row)) {
            return { language->lang_name, fmt_language_names[row - 1].Get() };
        }
        return { system_language_text, fmt_text.Get() };
    }

    // 開いたときと言語が変わったときに、文言と書式から組み直す。
    void Rebuild();
    void CreateTextFormats();
    void ComputeLayout();
    // 押せるのは言語の行だけなので、当たった行を返す。
    std::optional<size_t> HitTest(float x, float y) const noexcept;
    void Activate(size_t row);

    // PopupWindow から呼ばれる。
    void PaintContent();
    bool UpdateHover(float x, float y);
    void OnPress(UINT msg, float x, float y);

    std::optional<size_t> hovered;
    std::function<void(const i18n::Strings*)> on_language;

    // 「OS の設定に合わせる (日本語)」のように、今 OS に合わせると何語になるかを添える。
    std::wstring system_language_text;
    Layout layout{};

    Microsoft::WRL::ComPtr<IDWriteTextFormat> fmt_title;
    // 言語名はそれぞれの言語のロケールで組む。i18n::kAllStrings の並び。
    std::array<Microsoft::WRL::ComPtr<IDWriteTextFormat>, std::size(i18n::kAllStrings)> fmt_language_names;
};
