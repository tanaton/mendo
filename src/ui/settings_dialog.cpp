#include "settings_dialog_impl.h"
#include <algorithm>
#include <cmath>
#include <format>

using namespace settings_dialog_constants;

std::wstring FormatByteSize(uint64_t bytes)
{
    if (bytes < 1024) {
        return std::format(L"{} B", bytes);
    }
    constexpr const wchar_t* kUnits[] = { L"KB", L"MB", L"GB", L"TB" };
    double value = static_cast<double>(bytes) / 1024.0;
    size_t unit = 0;
    while (value >= 1024.0 && unit + 1 < std::size(kUnits)) {
        value /= 1024.0;
        ++unit;
    }
    // 桁が少ないときだけ小数を出す (「12.3 MB」「512 KB」)。
    return value < 100.0 ? std::format(L"{:.1f} {}", value, kUnits[unit]) : std::format(L"{:.0f} {}", value, kUnits[unit]);
}

void SettingsDialog::Init(ID2D1Factory* d2d_factory, IDWriteFactory* dwrite_factory)
{
    d2d_factory_ = d2d_factory;
    dwrite_factory_ = dwrite_factory;
}

void SettingsDialog::Impl::CreateTextFormats()
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

void SettingsDialog::Impl::Rebuild()
{
    const auto& ls = i18n::S();
    system_language_text = std::format(L"{} ({})", ls.settings_language_system, i18n::SystemStrings().lang_name);
    cache_size_text = FormatByteSize(cache_bytes);
    CreateTextFormats();
    ComputeLayout();
}

void SettingsDialog::Impl::ComputeLayout()
{
    const auto& ls = i18n::S();
    const float button_w = std::max(BUTTON_MIN_WIDTH, Measure(ls.settings_clear_cache, fmt_text.Get()) + 2 * BUTTON_PAD_X);

    float content_w = std::max({
        Measure(ls.settings_title, fmt_title.Get()),
        Measure(ls.settings_language, fmt_text.Get()),
        Measure(ls.settings_diagram_cache, fmt_text.Get()),
        Measure(cache_size_text, fmt_text.Get()) + BUTTON_GAP + button_w,
    });
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
    y += SECTION_GAP;
    l.separator_y = y;
    y += SECTION_GAP;
    l.cache_heading = { PAD_X, y, w - PAD_X, y + HEADING_HEIGHT };
    y += HEADING_HEIGHT;
    l.clear = { w - PAD_X - button_w, y, w - PAD_X, y + ROW_HEIGHT };
    l.cache_size = { PAD_X, y, l.clear.left - BUTTON_GAP, y + ROW_HEIGHT };
    height = y + ROW_HEIGHT + BOTTOM_PAD;
    layout = l;
}

SettingsDialog::Impl::Hit SettingsDialog::Impl::HitTest(float x, float y) const noexcept
{
    for (size_t row = 0; row < layout.languages.size(); ++row) {
        if (PointInRect(x, y, layout.languages[row])) {
            return { HitKind::Language, row };
        }
    }
    if (CanClearCache() && PointInRect(x, y, layout.clear)) {
        return { HitKind::ClearCache };
    }
    return {};
}

void SettingsDialog::Impl::Activate(const Hit& hit)
{
    switch (hit.kind) {
    case HitKind::None:
        return;
    case HitKind::Language: {
        const i18n::Strings* const chosen = LanguageOfRow(hit.row);
        if (chosen == i18n::ChosenLanguage() || !on_language) {
            return;
        }
        on_language(chosen);
        Rebuild();
        break;
    }
    case HitKind::ClearCache:
        on_clear_cache();
        cache_bytes = 0;
        cache_size_text = FormatByteSize(cache_bytes);
        break;
    }
    // 言語で幅が変わるので、今の中心のまま大きさを合わせる。テストはウィンドウを作らずに呼ぶ。
    if (hit.kind == HitKind::Language && hwnd) {
        RECT rc{};
        GetWindowRect(hwnd, &rc);
        Place(CenteredIn(rc));
    }
}

bool SettingsDialog::Impl::UpdateHover(float x, float y)
{
    const Hit hit = HitTest(x, y);
    if (hit == hovered) {
        return false;
    }
    hovered = hit;
    return true;
}

void SettingsDialog::Impl::OnPress(UINT msg, float x, float y)
{
    if (msg == WM_LBUTTONDOWN) {
        Activate(HitTest(x, y));
    }
}

void SettingsDialog::Show(HWND owner_hwnd, const SettingsDialogParams& params)
{
    Impl s;
    if (!s.Setup(d2d_factory_, dwrite_factory_, params.theme, params.dpi_scale)) {
        return;
    }
    s.cache_bytes = params.diagram_cache_bytes;
    s.on_language = params.on_language;
    s.on_clear_cache = params.on_clear_cache;
    s.Rebuild();
    RECT owner_rc{};
    GetWindowRect(owner_hwnd, &owner_rc);
    s.RunModal(owner_hwnd, i18n::S().settings_title, s.CenteredIn(owner_rc));
}

void SettingsDialog::Impl::PaintContent()
{
    const auto& ls = i18n::S();
    const float w = width;

    DrawLabel(ls.settings_title, fmt_title.Get(), { PAD_X, 0.0f, w - PAD_X, TITLE_HEIGHT }, brushes.text.Get());

    DrawLabel(ls.settings_language, fmt_text.Get(), layout.language_heading, brushes.sub.Get());
    const i18n::Strings* const chosen = i18n::ChosenLanguage();
    for (size_t row = 0; row < layout.languages.size(); ++row) {
        const DipRect& rect = layout.languages[row];
        if (hovered == Hit{ HitKind::Language, row }) {
            FillHover({ HOVER_MARGIN_X, rect.top + 1.0f, w - HOVER_MARGIN_X, rect.bottom - 1.0f }, HOVER_CORNER);
        }
        if (LanguageOfRow(row) == chosen) {
            DrawLabel(GLYPH_CHECKMARK, fmt_icon.Get(), { PAD_X - 4.0f, rect.top, PAD_X + CHECK_WIDTH - 8.0f, rect.bottom }, brushes.check.Get());
        }
        const auto [text, fmt] = LabelOfRow(row);
        DrawLabel(text, fmt, { PAD_X + CHECK_WIDTH, rect.top, w - PAD_X, rect.bottom }, brushes.text.Get());
    }

    rt->DrawLine({ PAD_X, layout.separator_y }, { w - PAD_X, layout.separator_y }, brushes.border.Get(), 1.0f);

    DrawLabel(ls.settings_diagram_cache, fmt_text.Get(), layout.cache_heading, brushes.sub.Get());
    DrawLabel(cache_size_text, fmt_text.Get(), layout.cache_size, brushes.text.Get());
    // 押せないボタンには HitTest が当たらないので、ホバーは押せるときだけ出る。
    if (hovered.kind == HitKind::ClearCache) {
        FillHover(layout.clear, HOVER_CORNER);
    }
    const D2D1_RECT_F button_border{ layout.clear.left + 0.5f, layout.clear.top + 0.5f, layout.clear.right - 0.5f, layout.clear.bottom - 0.5f };
    rt->DrawRoundedRectangle({ button_border, HOVER_CORNER, HOVER_CORNER }, brushes.border.Get(), 1.0f);
    DrawLabel(ls.settings_clear_cache, fmt_text_center.Get(), layout.clear, CanClearCache() ? brushes.text.Get() : brushes.gray.Get());
}
