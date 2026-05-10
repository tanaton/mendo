#include "renderer.h"
#include "d2d_util.h"
#include "log_hr.h"
#include "resource.h"
#include "ui_constants.h"
#include "wic_util.h"
#include <utility>

using Microsoft::WRL::ComPtr;

void Renderer::LoadAppIconBitmap()
{
    app_icon_bitmap_.Reset();

    const HMODULE hModule = GetModuleHandleW(nullptr);
    const HICON hIcon = static_cast<HICON>(LoadImageW(hModule, MAKEINTRESOURCEW(IDI_APP_ICON), IMAGE_ICON, 32, 32, LR_DEFAULTCOLOR));
    if (!hIcon) {
        OutputDebugStringW(L"[mendo] LoadAppIconBitmap: LoadImageW(IDI_APP_ICON) failed\n");
        return;
    }

    auto* wic = backend_.GetWICFactory();
    if (!wic) {
        DestroyIcon(hIcon);
        return;
    }

    ComPtr<IWICBitmap> wic_bitmap;
    HRESULT hr = wic->CreateBitmapFromHICON(hIcon, &wic_bitmap);
    DestroyIcon(hIcon);
    if (FAILED(hr)) {
        mendo::LogHrFailure(L"LoadAppIconBitmap: CreateBitmapFromHICON", hr);
        return;
    }

    auto converter = wic_util::ConvertBitmapSource(wic, wic_bitmap.Get());
    if (!converter) {
        OutputDebugStringW(L"[mendo] LoadAppIconBitmap: ConvertBitmapSource failed\n");
        return;
    }

    hr = rt()->CreateBitmapFromWicBitmap(converter.Get(), nullptr, &app_icon_bitmap_);
    if (FAILED(hr)) {
        mendo::LogHrFailure(L"LoadAppIconBitmap: CreateBitmapFromWicBitmap", hr);
    }
}

void Renderer::RecreateBrushes()
{
    auto* render_target_ = backend_.GetRenderTarget();
    if (!render_target_) {
        return;
    }

    const bool is_dark = theme_.IsDark();

    const float stripe_alpha = is_dark ? TABLE_STRIPE_ALPHA_DARK : TABLE_STRIPE_ALPHA_LIGHT;
    const float thumb_alpha = is_dark ? 0.4f : 0.25f;

    struct BrushSpec {
        BrushId id;
        D2D1_COLOR_F color;
    };
    BrushSpec specs[] = {
        { BrushId::Text, theme_.text_color },
        { BrushId::Heading, theme_.heading_color },
        { BrushId::CodeBg, theme_.code_bg_color },
        { BrushId::CodeText, theme_.code_text_color },
        { BrushId::Link, theme_.link_color },
        { BrushId::Hr, theme_.hr_color },
        { BrushId::BlockquoteBar, theme_.blockquote_bar_color },
        { BrushId::BlockquoteText, theme_.blockquote_text_color },
        { BrushId::Selection, SELECTION_COLOR },
        { BrushId::TableStripe, mendo::MonochromeOverlay(is_dark, stripe_alpha) },
        { BrushId::SyntaxKeyword, theme_.syntax_keyword },
        { BrushId::SyntaxType, theme_.syntax_type },
        { BrushId::SyntaxString, theme_.syntax_string },
        { BrushId::SyntaxNumber, theme_.syntax_number },
        { BrushId::SyntaxComment, theme_.syntax_comment },
        { BrushId::SyntaxPreprocessor, theme_.syntax_preprocessor },
        { BrushId::SyntaxFunction, theme_.syntax_function },
        { BrushId::AlertNote, theme_.alert_color[0] },
        { BrushId::AlertTip, theme_.alert_color[1] },
        { BrushId::AlertImportant, theme_.alert_color[2] },
        { BrushId::AlertWarning, theme_.alert_color[3] },
        { BrushId::AlertCaution, theme_.alert_color[4] },
        { BrushId::TitleBarBg, theme_.titlebar_bg_color },
        { BrushId::TitleBarText, theme_.titlebar_text_color },
        { BrushId::TitleBarButtonHover, theme_.titlebar_button_hover_color },
        { BrushId::TitleBarButtonActive, theme_.titlebar_button_active_color },
        { BrushId::TitleBarCloseRed, D2D1::ColorF(0xE81123) },
        { BrushId::TitleBarCloseWhite, D2D1::ColorF(D2D1::ColorF::White) },
        { BrushId::PaneBg, theme_.pane_bg_color },
        { BrushId::Splitter, theme_.splitter_color },
        { BrushId::PaneItemHover, theme_.pane_item_hover_color },
        { BrushId::PaneItemActive, theme_.pane_item_active_color },
        { BrushId::ScrollbarThumb, mendo::MonochromeOverlay(is_dark, thumb_alpha) },
        { BrushId::Overlay, D2D1::ColorF(0, 0, 0, 1.0f) },
        { BrushId::OverlayWhite, D2D1::ColorF(1.0f, 1.0f, 1.0f, 1.0f) },
        { BrushId::OverlayBlack, D2D1::ColorF(0.0f, 0.0f, 0.0f, 1.0f) },
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
    for (const auto& s : specs) {
        auto& brush = brushes_[std::to_underlying(s.id)];
        if (brush) {
            brush->SetColor(s.color);
            continue;
        }
        mendo::CreateSolidColorBrushOrFallback(render_target_, s.color, brush);
    }
}

void Renderer::InvalidateBrushes() noexcept
{
    for (auto& b : brushes_) {
        b.Reset();
    }
}

ComPtr<IDWriteTextFormat> Renderer::CreatePaneFormat(const wchar_t* family, DWRITE_FONT_WEIGHT weight, float size, const wchar_t* locale)
{
    auto* dw = backend_.GetDWriteFactory();
    ComPtr<IDWriteTextFormat> fmt;
    HRESULT hr = dw->CreateTextFormat(family, nullptr, weight, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, size, locale, &fmt);
    if (SUCCEEDED(hr)) {
        return fmt;
    }
    // 無効なフォントファミリ名はフォールバック (Segoe UI) で再試行。
    // ユーザに「フォントが効かない」状態を出さないための最終防壁。
    mendo::LogHrFailure(L"CreateTextFormat (falling back to Segoe UI)", hr);
    fmt.Reset();
    hr = dw->CreateTextFormat(L"Segoe UI", nullptr, weight, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, size, locale, &fmt);
    if (FAILED(hr)) {
        mendo::LogHrFailure(L"CreateTextFormat (Segoe UI fallback)", hr);
    }
    return fmt;
}

void Renderer::RecreatePaneFormats()
{
    constexpr DWRITE_FONT_WEIGHT W = DWRITE_FONT_WEIGHT_NORMAL;
    constexpr DWRITE_TEXT_ALIGNMENT TA_LEAD = DWRITE_TEXT_ALIGNMENT_LEADING;
    constexpr DWRITE_TEXT_ALIGNMENT TA_CTR = DWRITE_TEXT_ALIGNMENT_CENTER;
    constexpr DWRITE_TEXT_ALIGNMENT TA_TAIL = DWRITE_TEXT_ALIGNMENT_TRAILING;
    constexpr DWRITE_PARAGRAPH_ALIGNMENT PA_TOP = DWRITE_PARAGRAPH_ALIGNMENT_NEAR;
    constexpr DWRITE_PARAGRAPH_ALIGNMENT PA_CTR = DWRITE_PARAGRAPH_ALIGNMENT_CENTER;

    const wchar_t* const body_font = theme_.font_family.c_str();
    const wchar_t* const icon_font = L"Segoe Fluent Icons";

    struct FormatSpec {
        ComPtr<IDWriteTextFormat>* target;
        const wchar_t* family;
        DWRITE_FONT_WEIGHT weight;
        float size;
        const wchar_t* locale;
        DWRITE_TEXT_ALIGNMENT text_align;
        DWRITE_PARAGRAPH_ALIGNMENT para_align;
        bool no_wrap;
    };

    FormatSpec specs[] = {
        { &fmt_.icon_font,        body_font, W,                            theme_.font_size_body,        L"ja-jp", TA_LEAD, PA_TOP, false },
        { &fmt_.copy_btn_icon,    icon_font, W,                            theme_.font_size_body,        L"en-us", TA_CTR,  PA_CTR, true  },
        { &fmt_.list_number,      body_font, W,                            theme_.font_size_body,        L"ja-jp", TA_TAIL, PA_TOP, false },
        { &fmt_.placeholder_text, body_font, W,                            theme_.font_size_body,        L"ja-jp", TA_CTR,  PA_CTR, false },
        { &fmt_.titlebar_text,    body_font, W,                            TITLEBAR_TEXT_FONT_SIZE,      L"ja-jp", TA_CTR,  PA_CTR, true  },
        { &fmt_.titlebar_icon,    icon_font, W,                            14.0f,                        L"en-us", TA_CTR,  PA_CTR, true  },
        { &fmt_.pane_icon,        icon_font, W,                            theme_.pane_font_size,        L"en-us", TA_CTR,  PA_CTR, true  },
        { &fmt_.pane_item,        body_font, W,                            theme_.pane_font_size,        L"ja-jp", TA_LEAD, PA_CTR, true  },
        { &fmt_.pane_header,      body_font, DWRITE_FONT_WEIGHT_SEMI_BOLD, theme_.pane_font_size,        L"ja-jp", TA_LEAD, PA_CTR, true  },
        { &fmt_.nav_button,       body_font, W,                            theme_.pane_font_size,        L"ja-jp", TA_CTR,  PA_CTR, true  },
        { &fmt_.gesture_overlay,  body_font, DWRITE_FONT_WEIGHT_BOLD,      32.0f * theme_.zoom,          L"ja-JP", TA_CTR,  PA_CTR, false },
        { &fmt_.toast_text,       body_font, DWRITE_FONT_WEIGHT_SEMI_BOLD, theme_.pane_font_size * 1.1f, L"ja-JP", TA_CTR,  PA_CTR, true  },
        { &fmt_.search_input,     body_font, W,                            theme_.pane_font_size,        L"ja-jp", TA_LEAD, PA_CTR, true  },
        { &fmt_.search_count,     body_font, W,                            theme_.pane_font_size * 0.9f, L"ja-jp", TA_CTR,  PA_CTR, true  },
        { &fmt_.search_icon,      icon_font, W,                            14.0f,                        L"en-us", TA_CTR,  PA_CTR, true  },
    };

    for (const auto& s : specs) {
        *s.target = CreatePaneFormat(s.family, s.weight, s.size, s.locale);
        if (*s.target) {
            if (s.no_wrap) {
                (*s.target)->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
            }
            if (s.text_align != TA_LEAD) {
                (*s.target)->SetTextAlignment(s.text_align);
            }
            if (s.para_align != PA_TOP) {
                (*s.target)->SetParagraphAlignment(s.para_align);
            }
        }
    }

    // 全レイアウトは IDWriteTextFormat にバインドされているため format 再作成と同時に破棄する。
    nav_back_layout_.Reset();
    nav_forward_layout_.Reset();
    gesture_back_layout_.Reset();
    gesture_forward_layout_.Reset();
    cached_toast_layout_.Reset();
    cached_toast_text_.clear();
    cached_search_layout_.Reset();
    cached_search_text_.clear();
    cached_search_query_.clear();
    cached_search_ime_comp_.clear();
    cached_search_caret_pos_ = -1;
    cached_search_width_ = -1.0f;
    cached_search_has_underline_ = false;
    cached_search_effective_pos_ = -2;
    cached_search_caret_x_ = 0.0f;

    file_pane_cache_.Reset();
    toc_pane_cache_.Reset();

    cmd_generator_.SetFormats({ fmt_.list_number.Get(), fmt_.icon_font.Get(), fmt_.copy_btn_icon.Get(), fmt_.placeholder_text.Get() });

    // ナビ/ジェスチャー用レイアウトを eager 作成。描画ホットパス上の null 分岐を排除する。
    auto* dw = backend_.GetDWriteFactory();
    if (dw) {
        if (fmt_.nav_button) {
            static constexpr wchar_t BACK_ICON[] = L"\x25C0";
            static constexpr wchar_t FORWARD_ICON[] = L"\x25B6";
            dw->CreateTextLayout(BACK_ICON, 1, fmt_.nav_button.Get(), NAV_BTN_SIZE, NAV_BTN_SIZE, &nav_back_layout_);
            dw->CreateTextLayout(FORWARD_ICON, 1, fmt_.nav_button.Get(), NAV_BTN_SIZE, NAV_BTN_SIZE, &nav_forward_layout_);
        }
        if (fmt_.gesture_overlay) {
            static constexpr wchar_t GESTURE_BACK[] = L"\x2190 \x623B\x308B";
            static constexpr wchar_t GESTURE_FORWARD[] = L"\x2192 \x9032\x3080";
            dw->CreateTextLayout(GESTURE_BACK, 4, fmt_.gesture_overlay.Get(), 280.0f, 80.0f, &gesture_back_layout_);
            dw->CreateTextLayout(GESTURE_FORWARD, 4, fmt_.gesture_overlay.Get(), 280.0f, 80.0f, &gesture_forward_layout_);
        }
    }
}
