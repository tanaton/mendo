#include "renderer.h"
#include "d2d_util.h"
#include "i18n.h"
#include "resource.h"
#include "ui_constants.h"
#include "utility.h"
#include "wic_util.h"
#include "win32_util.h"
#include "profiler.h"
#include <utility>

using Microsoft::WRL::ComPtr;

void Renderer::DecodeAppIcon()
{
    const HMODULE hModule = GetModuleHandleW(nullptr);
    const HICON hIcon = static_cast<HICON>(LoadImageW(hModule, MAKEINTRESOURCEW(IDI_APP_ICON), IMAGE_ICON, 32, 32, LR_DEFAULTCOLOR));
    if (!hIcon) {
        MENDO_TRACE("DecodeAppIcon: LoadImageW(IDI_APP_ICON) failed");
        return;
    }
    const auto icon_guard = ScopeGuard([hIcon] { DestroyIcon(hIcon); });

    auto* wic = backend_.GetWICFactory();
    if (!wic) {
        return;
    }

    ComPtr<IWICBitmap> wic_bitmap;
    HRESULT hr = wic->CreateBitmapFromHICON(hIcon, &wic_bitmap);
    if (FAILED(hr)) {
        LogHrFailure("DecodeAppIcon: CreateBitmapFromHICON", hr);
        return;
    }

    auto converter = wic_util::ConvertBitmapSource(wic, wic_bitmap.Get());
    if (!converter) {
        MENDO_TRACE("DecodeAppIcon: ConvertBitmapSource failed");
        return;
    }

    // 変換をここで確定させ、デバイス作成後の処理を D2D ビットマップへの転送だけにする。
    hr = wic->CreateBitmapFromSource(converter.Get(), WICBitmapCacheOnLoad, &app_icon_wic_);
    if (FAILED(hr)) {
        LogHrFailure("DecodeAppIcon: CreateBitmapFromSource", hr);
    }
}

void Renderer::LoadAppIconBitmap()
{
    app_icon_bitmap_.Reset();
    if (!app_icon_wic_) {
        return;
    }
    const HRESULT hr = rt()->CreateBitmapFromWicBitmap(app_icon_wic_.Get(), nullptr, &app_icon_bitmap_);
    if (FAILED(hr)) {
        LogHrFailure("LoadAppIconBitmap: CreateBitmapFromWicBitmap", hr);
    }
}

void Renderer::RecreateBrushes()
{
    auto* const target = rt();
    if (!target) {
        return;
    }

    const bool is_dark = theme_.IsDark();
    const float thumb_alpha = is_dark ? 0.4f : 0.25f;

    struct BrushSpec {
        BrushId id;
        D2D1_COLOR_F color;
    };
    const BrushSpec specs[] = {
        { BrushId::Text, theme_.text_color },
        { BrushId::Heading, theme_.heading_color },
        { BrushId::CodeBg, theme_.code_bg_color },
        { BrushId::InlineCodeBg, theme_.inline_code_bg_color },
        { BrushId::CodeText, theme_.code_text_color },
        { BrushId::Link, theme_.link_color },
        { BrushId::Hr, theme_.hr_color },
        { BrushId::BlockquoteBar, theme_.blockquote_bar_color },
        { BrushId::BlockquoteText, theme_.blockquote_text_color },
        { BrushId::Selection, SELECTION_COLOR },
        { BrushId::TableStripe, TableStripeColor(is_dark) },
        { BrushId::SyntaxKeyword, theme_.syntax_keyword },
        { BrushId::SyntaxType, theme_.syntax_type },
        { BrushId::SyntaxString, theme_.syntax_string },
        { BrushId::SyntaxNumber, theme_.syntax_number },
        { BrushId::SyntaxComment, theme_.syntax_comment },
        { BrushId::SyntaxPreprocessor, theme_.syntax_preprocessor },
        { BrushId::SyntaxFunction, theme_.syntax_function },
        { BrushId::SyntaxInserted, theme_.syntax_inserted },
        { BrushId::SyntaxDeleted, theme_.syntax_deleted },
        { BrushId::TitleBarBg, theme_.titlebar_bg_color },
        { BrushId::TitleBarText, theme_.titlebar_text_color },
        { BrushId::TitleBarButtonHover, theme_.titlebar_button_hover_color },
        { BrushId::TitleBarButtonActive, theme_.titlebar_button_active_color },
        { BrushId::TitleBarCloseRed, D2D1::ColorF(0xE81123) },
        { BrushId::Splitter, theme_.splitter_color },
        { BrushId::PaneBg, theme_.pane_bg_color },
        { BrushId::PaneItemHover, theme_.pane_item_hover_color },
        { BrushId::PaneItemActive, theme_.pane_item_active_color },
        { BrushId::ScrollbarThumb, MonochromeOverlay(is_dark, thumb_alpha) },
        { BrushId::OverlayWhite, D2D1::ColorF(1.0f, 1.0f, 1.0f, 1.0f) },
        { BrushId::OverlayBlack, D2D1::ColorF(0.0f, 0.0f, 0.0f, 1.0f) },
        { BrushId::OverlayGestureBg, D2D1::ColorF(0.2f, 0.2f, 0.2f, 1.0f) },
        { BrushId::GestureTrail, D2D1::ColorF(0.9f, 0.2f, 0.2f, 1.0f) },
        { BrushId::SearchBarBg, theme_.search_bar_bg_color },
        { BrushId::SearchBarBorder, theme_.search_bar_border_color },
        { BrushId::SearchInputBg, theme_.search_input_bg_color },
        { BrushId::SearchInputText, theme_.search_input_text_color },
        { BrushId::SearchHighlight, theme_.search_highlight_color },
        { BrushId::SearchHighlightCurrent, theme_.search_highlight_current_color },
        { BrushId::SearchNoMatchBg, theme_.search_no_match_bg_color },
    };

    // 既存ブラシには SetColor のみ、未生成のものだけ CreateSolidColorBrush。
    // テーマ切替時の 40+ 個の COM オブジェクト再生成を回避する。
    const auto apply = [&](BrushId id, D2D1_COLOR_F color) {
        auto& brush = brushes_[std::to_underlying(id)];
        if (brush) {
            brush->SetColor(color);
            return;
        }
        CreateSolidColorBrushOrFallback(target, color, brush);
    };
    for (const auto& s : specs) {
        apply(s.id, s.color);
    }
    for (size_t i = 0; i < ALERT_BRUSH_COUNT; ++i) {
        apply(AlertBrushIdFromIndex(i), theme_.alert_color[i]);
        apply(AlertBgBrushIdFromIndex(i), theme_.alert_bg_color[i]);
    }
}

void Renderer::InvalidateBrushes() noexcept
{
    for (auto& b : brushes_) {
        b.Reset();
    }
}

namespace {
std::wstring ResolveFontFamily(IDWriteFontCollection* collection, std::initializer_list<const wchar_t*> candidates)
{
    if (collection) {
        for (const wchar_t* name : candidates) {
            UINT32 index = 0;
            BOOL exists = FALSE;
            if (SUCCEEDED(collection->FindFamilyName(name, &index, &exists)) && exists) {
                return name;
            }
        }
    }
    return *(candidates.end() - 1);
}
} // namespace

void Renderer::ResolveThemeFonts()
{
    auto* dw = backend_.GetDWriteFactory();
    ComPtr<IDWriteFontCollection> collection;
    if (dw) {
        dw->GetSystemFontCollection(&collection, FALSE);
    }
    theme_.monospace_font = ResolveFontFamily(collection.Get(), { theme_.monospace_font.c_str(), L"Consolas", L"Courier New" });
    theme_.icon_font = ResolveFontFamily(collection.Get(), { theme_.icon_font.c_str(), L"Segoe Fluent Icons", L"Segoe MDL2 Assets" });
}

void Renderer::AdoptThemeKeepingFonts(Theme theme)
{
    theme.monospace_font = std::move(theme_.monospace_font);
    theme.icon_font = std::move(theme_.icon_font);
    theme_ = std::move(theme);
}

void Renderer::EnsurePaneListFormats(PaneTarget target, float zoom)
{
    auto& f = pane_list_fmt_[std::to_underlying(target)];
    if (f.zoom == zoom) {
        return;
    }
    auto* const dw = backend_.GetDWriteFactory();
    const float size = theme_.pane_font_size * zoom;
    f.item = CreateTextFormat(dw, { .family = theme_.font_family.c_str(), .size = size, .locale = theme_.text_locale, .para_align = DWRITE_PARAGRAPH_ALIGNMENT_CENTER, .no_wrap = true });
    // アイコンはファイル一覧だけ。
    if (target == PaneTarget::File) {
        f.icon = CreateTextFormat(dw, { .family = theme_.icon_font.c_str(), .size = size, .text_align = DWRITE_TEXT_ALIGNMENT_CENTER, .para_align = DWRITE_PARAGRAPH_ALIGNMENT_CENTER, .no_wrap = true });
    }
    f.zoom = zoom;
}

void Renderer::RecreateAllTextFormats()
{
    CreateTextFormats(/*ui_only=*/false);

    for (auto& f : pane_list_fmt_) {
        f = {};
    }

    // 古い書式のレイアウトは二度と引かれないので、まとめて捨てる。
    text_layouts_.SetFactory(backend_.GetDWriteFactory());
    search_input_ = {};
}

void Renderer::CreateTextFormats(bool ui_only)
{
    constexpr DWRITE_FONT_WEIGHT W = DWRITE_FONT_WEIGHT_NORMAL;
    constexpr DWRITE_FONT_WEIGHT SEMI = DWRITE_FONT_WEIGHT_SEMI_BOLD;
    constexpr DWRITE_TEXT_ALIGNMENT TA_LEAD = DWRITE_TEXT_ALIGNMENT_LEADING;
    constexpr DWRITE_TEXT_ALIGNMENT TA_CTR = DWRITE_TEXT_ALIGNMENT_CENTER;
    constexpr DWRITE_TEXT_ALIGNMENT TA_TAIL = DWRITE_TEXT_ALIGNMENT_TRAILING;
    constexpr DWRITE_PARAGRAPH_ALIGNMENT PA_TOP = DWRITE_PARAGRAPH_ALIGNMENT_NEAR;
    constexpr DWRITE_PARAGRAPH_ALIGNMENT PA_CTR = DWRITE_PARAGRAPH_ALIGNMENT_CENTER;

    const wchar_t* const body_font = theme_.font_family.c_str();
    const wchar_t* const icon_font = theme_.icon_font.c_str();

    // i18n::TextLocale の文書から来る文字は Doc、UI の決まった文言は Ui のロケールで組む。アイコンは En。
    enum class Loc : uint8_t {
        Doc,
        Ui,
        En,
    };
    struct FormatSpec {
        // ComPtr の & は中身を解放するので、参照で持つ (ui_only のとき作り直さない書式まで消える)。
        ComPtr<IDWriteTextFormat>& target;
        Loc loc;
        // locale は loc から決める。
        TextFormatSpec spec;
    };

    // clang-format off
    const FormatSpec specs[] = {
        { fmt_.checkbox,          Loc::Doc,  { body_font, W,                       theme_.font_size_body,        nullptr, TA_LEAD, PA_TOP, false, BODY_LINE_HEIGHT_FACTOR } },
        { fmt_.copy_btn_icon,     Loc::En,   { icon_font, W,                       theme_.font_size_body,        nullptr, TA_CTR,  PA_CTR, true  } },
        { fmt_.list_number,       Loc::Doc,  { body_font, W,                       theme_.font_size_body,        nullptr, TA_TAIL, PA_TOP, false, BODY_LINE_HEIGHT_FACTOR } },
        { fmt_.placeholder_text,  Loc::Ui,   { body_font, W,                       theme_.font_size_body,        nullptr, TA_CTR,  PA_CTR, false } },
        { fmt_.titlebar_text,     Loc::Doc,  { body_font, W,                       TITLEBAR_TEXT_FONT_SIZE,      nullptr, TA_CTR,  PA_CTR, true  } },
        { fmt_.ui_icon,           Loc::En,   { icon_font, W,                       UI_ICON_FONT_SIZE,            nullptr, TA_CTR,  PA_CTR, true  } },
        { fmt_.pane_icon,         Loc::En,   { icon_font, W,                       theme_.pane_font_size,        nullptr, TA_CTR,  PA_CTR, true  } },
        { fmt_.pane_header,       Loc::Ui,   { body_font, SEMI,                    theme_.pane_font_size,        nullptr, TA_LEAD, PA_CTR, true  } },
        { fmt_.nav_button,        Loc::Doc,  { body_font, W,                       theme_.pane_font_size,        nullptr, TA_CTR,  PA_CTR, true  } },
        { fmt_.gesture_overlay,   Loc::Ui,   { body_font, DWRITE_FONT_WEIGHT_BOLD, 32.0f * theme_.zoom,          nullptr, TA_CTR,  PA_CTR, false } },
        { fmt_.toast_text,        Loc::Ui,   { body_font, SEMI,                    theme_.pane_font_size * 1.1f, nullptr, TA_CTR,  PA_CTR, true  } },
        { fmt_.search_input,      Loc::Doc,  { body_font, W,                       SEARCH_INPUT_FONT_SIZE,       nullptr, TA_LEAD, PA_CTR, true  } },
        { fmt_.search_count,      Loc::Doc,  { body_font, W,                       SEARCH_COUNT_FONT_SIZE,       nullptr, TA_CTR,  PA_CTR, true  } },
    };
    // clang-format on

    ui_fmt_locale_ = i18n::S().lang_locale;
    const auto locale_of = [this](Loc loc) noexcept {
        switch (loc) {
        case Loc::Doc:
            return theme_.text_locale;
        case Loc::Ui:
            return ui_fmt_locale_;
        case Loc::En:
            break;
        }
        return L"en-us";
    };
    auto* const dw = backend_.GetDWriteFactory();
    for (const auto& s : specs) {
        if (ui_only && s.loc != Loc::Ui) {
            continue;
        }
        auto spec = s.spec;
        spec.locale = locale_of(s.loc);
        s.target = CreateTextFormat(dw, spec);
    }

    cmd_generator_.SetFormats({ fmt_.list_number.Get(), fmt_.checkbox.Get(), fmt_.copy_btn_icon.Get(), fmt_.placeholder_text.Get() });
}
