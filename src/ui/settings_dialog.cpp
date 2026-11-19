#include "settings_dialog.h"
#include <algorithm>
#include <cmath>
#include <format>

using namespace settings_dialog_constants;

void SettingsDialog::CreateTextFormats()
{
    const wchar_t* const family = theme->font_family.c_str();
    const float size = theme->pane_font_size;
    constexpr auto CENTER = DWRITE_PARAGRAPH_ALIGNMENT_CENTER;
    // 言語名はロケールが決まっているので、言語を切り替えても作り直さない。
    if (!fmt_language_names[0]) {
        for (size_t i = 0; i < std::size(i18n::kAllStrings); ++i) {
            fmt_language_names[i] = CreateTextFormat(dwrite_factory, { .family = family, .size = size, .locale = i18n::kAllStrings[i]->lang_locale, .para_align = CENTER, .no_wrap = true });
        }
    }
    fmt_title = CreateTextFormat(dwrite_factory, { .family = family, .weight = DWRITE_FONT_WEIGHT_SEMI_BOLD, .size = size * TITLE_FONT_SCALE, .locale = i18n::S().lang_locale, .para_align = CENTER, .no_wrap = true });
    CreateBaseTextFormats(ICON_FONT_SIZE);
}

void SettingsDialog::Rebuild()
{
    const auto& ls = i18n::S();
    system_language_text = std::format(L"{} ({})", ls.settings_language_system, i18n::SystemStrings().lang_name);
    CreateTextFormats();
    ComputeLayout();
}

void SettingsDialog::ComputeLayout()
{
    const auto& ls = i18n::S();
    float content_w = std::max(Measure(ls.settings_title, fmt_title.Get()), Measure(ls.settings_language, fmt_text.Get()));
    for (size_t row = 0; row < kLanguageRowCount; ++row) {
        const auto [text, fmt] = LabelOfRow(row);
        content_w = std::max(content_w, CHECK_WIDTH + Measure(text, fmt));
    }

    Layout l;
    const float w = std::max(MIN_WIDTH, std::ceil(content_w + 2 * PAD_X));
    width = w;

    float y = TITLE_HEIGHT;
    l.language_heading = { PAD_X, y, w - PAD_X, y + HEADING_HEIGHT };
    y += HEADING_HEIGHT;
    for (auto& row : l.languages) {
        row = { 0.0f, y, w, y + ROW_HEIGHT };
        y += ROW_HEIGHT;
    }
    height = y + BOTTOM_PAD;
    layout = l;
}

std::optional<size_t> SettingsDialog::HitTest(float x, float y) const noexcept
{
    for (size_t row = 0; row < layout.languages.size(); ++row) {
        if (PointInRect(x, y, layout.languages[row])) {
            return row;
        }
    }
    return std::nullopt;
}

void SettingsDialog::Activate(size_t row)
{
    const i18n::Strings* const chosen = LanguageOfRow(row);
    if (chosen == i18n::ChosenLanguage()) {
        return;
    }
    on_language(chosen);
    Rebuild();
    // 言語で幅が変わるので、今の中心のまま大きさを合わせる。テストはウィンドウを作らずに呼ぶ。
    if (hwnd) {
        RECT rc{};
        GetWindowRect(hwnd, &rc);
        Place(CenteredIn(rc));
    }
}

bool SettingsDialog::UpdateHover(float x, float y)
{
    const auto hit = HitTest(x, y);
    if (hit == hovered) {
        return false;
    }
    hovered = hit;
    return true;
}

void SettingsDialog::OnPress(UINT msg, float x, float y)
{
    if (msg != WM_LBUTTONDOWN) {
        return;
    }
    if (const auto row = HitTest(x, y)) {
        Activate(*row);
    }
}

void SettingsDialog::Show(HWND owner_hwnd, const SettingsDialogParams& params)
{
    SettingsDialog s;
    if (!s.Setup(params.env)) {
        return;
    }
    s.on_language = params.on_language;
    s.Rebuild();
    RECT owner_rc{};
    GetWindowRect(owner_hwnd, &owner_rc);
    s.RunModal(owner_hwnd, i18n::S().settings_title, s.CenteredIn(owner_rc));
}

void SettingsDialog::PaintContent()
{
    const auto& ls = i18n::S();
    const float w = width;

    DrawLabel(ls.settings_title, fmt_title.Get(), { PAD_X, 0.0f, w - PAD_X, TITLE_HEIGHT }, brushes.text.Get());

    DrawLabel(ls.settings_language, fmt_text.Get(), layout.language_heading, brushes.sub.Get());
    const i18n::Strings* const chosen = i18n::ChosenLanguage();
    for (size_t row = 0; row < layout.languages.size(); ++row) {
        const DipRect& rect = layout.languages[row];
        if (hovered == row) {
            FillHover({ HOVER_MARGIN_X, rect.top + 1.0f, w - HOVER_MARGIN_X, rect.bottom - 1.0f });
        }
        if (LanguageOfRow(row) == chosen) {
            DrawCheck({ PAD_X - 4.0f, rect.top, PAD_X + CHECK_WIDTH - 8.0f, rect.bottom });
        }
        const auto [text, fmt] = LabelOfRow(row);
        DrawLabel(text, fmt, { PAD_X + CHECK_WIDTH, rect.top, w - PAD_X, rect.bottom }, brushes.text.Get());
    }
}
