#pragma once
// SettingsDialog の private 実装ヘッダ。settings_dialog.cpp とテストの両方からインクルードする。
#include "settings_dialog.h"
#include "i18n.h"
#include "popup_window.h"
#include <array>
#include <iterator>
#include <string>

namespace settings_dialog_constants {
inline constexpr float MIN_WIDTH = 320.0f;
inline constexpr float PAD_X = 16.0f;
inline constexpr float TITLE_HEIGHT = 44.0f;
inline constexpr float TITLE_FONT_SCALE = 1.25f;
inline constexpr float HEADING_HEIGHT = 24.0f;
inline constexpr float ROW_HEIGHT = 28.0f;
inline constexpr float CHECK_WIDTH = 24.0f;
inline constexpr float HOVER_MARGIN_X = 8.0f;
inline constexpr float SECTION_GAP = 10.0f;
inline constexpr float BUTTON_PAD_X = 14.0f;
inline constexpr float BUTTON_MIN_WIDTH = 72.0f;
inline constexpr float BUTTON_GAP = 16.0f;
inline constexpr float BOTTOM_PAD = 14.0f;
inline constexpr float ICON_FONT_SIZE = 12.0f;

// 言語の一覧の行。先頭は OS に合わせる (nullptr)、残りは i18n::kAllStrings の並び。
inline constexpr size_t kLanguageRowCount = std::size(i18n::kAllStrings) + 1;
} // namespace settings_dialog_constants

// "0 B" / "512 KB" / "12.3 MB" の形にする。
std::wstring FormatByteSize(uint64_t bytes);

struct SettingsDialog::Impl : PopupWindow<SettingsDialog::Impl> {
    static constexpr wchar_t kClassName[] = L"mendoSettingsDialog";

    enum class HitKind : uint8_t {
        None,
        Language,
        ClearCache,
    };
    struct Hit {
        HitKind kind = HitKind::None;
        // Language のときの行。
        size_t row = 0;
        bool operator==(const Hit&) const = default;
    };

    // 大きさは PopupWindow::width / height。
    struct Layout {
        DipRect language_heading{};
        std::array<DipRect, settings_dialog_constants::kLanguageRowCount> languages{};
        float separator_y = 0.0f;
        DipRect cache_heading{};
        DipRect cache_size{};
        DipRect clear{};
    };

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
    Hit HitTest(float x, float y) const noexcept;
    bool CanClearCache() const noexcept
    {
        return cache_bytes > 0 && static_cast<bool>(on_clear_cache);
    }
    void Activate(const Hit& hit);

    // PopupWindow から呼ばれる。
    void PaintContent();
    bool UpdateHover(float x, float y);
    void OnPress(UINT msg, float x, float y);

    Hit hovered{};
    uint64_t cache_bytes = 0;
    std::function<void(const i18n::Strings*)> on_language;
    std::function<void()> on_clear_cache;

    // 「OS の設定に合わせる (日本語)」のように、今 OS に合わせると何語になるかを添える。
    std::wstring system_language_text;
    std::wstring cache_size_text;
    Layout layout{};

    Microsoft::WRL::ComPtr<IDWriteTextFormat> fmt_title;
    // 言語名はそれぞれの言語のロケールで組む。i18n::kAllStrings の並び。
    std::array<Microsoft::WRL::ComPtr<IDWriteTextFormat>, std::size(i18n::kAllStrings)> fmt_language_names;
};
