#include "renderer.h"
#include "resource.h"
#include "ui_constants.h"
#include "wic_util.h"

using Microsoft::WRL::ComPtr;

void Renderer::LoadAppIconBitmap()
{
    app_icon_bitmap_.Reset();

    const HMODULE hModule = GetModuleHandleW(nullptr);
    const HICON hIcon = static_cast<HICON>(LoadImageW(hModule, MAKEINTRESOURCEW(IDI_APP_ICON), IMAGE_ICON, 32, 32, LR_DEFAULTCOLOR));
    if (!hIcon) {
        return;
    }

    // HICONからD2D1ビットマップに変換（バックエンドのWICファクトリを共有使用）
    auto* wic = backend_.GetWICFactory();
    if (!wic) {
        DestroyIcon(hIcon);
        return;
    }

    ComPtr<IWICBitmap> wic_bitmap;
    HRESULT hr = wic->CreateBitmapFromHICON(hIcon, &wic_bitmap);
    DestroyIcon(hIcon);
    if (FAILED(hr)) {
        return;
    }

    auto converter = wic_util::ConvertBitmapSource(wic, wic_bitmap.Get());
    if (!converter) {
        return;
    }

    rt()->CreateBitmapFromWicBitmap(converter.Get(), nullptr, &app_icon_bitmap_);
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

    struct BrushSpec { BrushId id; D2D1_COLOR_F color; };
    BrushSpec specs[] = {
        {BrushId::Text,             theme_.text_color},
        {BrushId::Heading,          theme_.heading_color},
        {BrushId::CodeBg,           theme_.code_bg_color},
        {BrushId::CodeText,         theme_.code_text_color},
        {BrushId::Link,             theme_.link_color},
        {BrushId::Hr,               theme_.hr_color},
        {BrushId::BlockquoteBar,    theme_.blockquote_bar_color},
        {BrushId::BlockquoteText,   theme_.blockquote_text_color},
        {BrushId::Selection,        SELECTION_COLOR},
        {BrushId::TableStripe,      is_dark ? D2D1::ColorF(1.0f, 1.0f, 1.0f, stripe_alpha)
                                            : D2D1::ColorF(0.0f, 0.0f, 0.0f, stripe_alpha)},
        {BrushId::SyntaxKeyword,    theme_.syntax_keyword},
        {BrushId::SyntaxType,       theme_.syntax_type},
        {BrushId::SyntaxString,     theme_.syntax_string},
        {BrushId::SyntaxNumber,     theme_.syntax_number},
        {BrushId::SyntaxComment,    theme_.syntax_comment},
        {BrushId::SyntaxPreprocessor, theme_.syntax_preprocessor},
        {BrushId::SyntaxFunction,   theme_.syntax_function},
        {BrushId::AlertNote,        theme_.alert_color[0]},
        {BrushId::AlertTip,         theme_.alert_color[1]},
        {BrushId::AlertImportant,   theme_.alert_color[2]},
        {BrushId::AlertWarning,     theme_.alert_color[3]},
        {BrushId::AlertCaution,     theme_.alert_color[4]},
        {BrushId::TitleBarBg,       theme_.titlebar_bg_color},
        {BrushId::TitleBarText,     theme_.titlebar_text_color},
        {BrushId::TitleBarButtonHover, theme_.titlebar_button_hover_color},
        {BrushId::TitleBarButtonActive, theme_.titlebar_button_active_color},
        {BrushId::TitleBarCloseRed,  D2D1::ColorF(0xE81123)},
        {BrushId::TitleBarCloseWhite, D2D1::ColorF(D2D1::ColorF::White)},
        {BrushId::PaneBg,           theme_.pane_bg_color},
        {BrushId::Splitter,         theme_.splitter_color},
        {BrushId::PaneItemHover,    theme_.pane_item_hover_color},
        {BrushId::PaneItemActive,   theme_.pane_item_active_color},
        {BrushId::ScrollbarThumb,   is_dark ? D2D1::ColorF(1.0f, 1.0f, 1.0f, thumb_alpha)
                                            : D2D1::ColorF(0.0f, 0.0f, 0.0f, thumb_alpha)},
        {BrushId::Overlay,          D2D1::ColorF(0, 0, 0, 1.0f)},
        {BrushId::SearchBarBg,      theme_.search_bar_bg_color},
        {BrushId::SearchBarBorder,  theme_.search_bar_border_color},
        {BrushId::SearchInputBg,    theme_.search_input_bg_color},
        {BrushId::SearchInputText,  theme_.search_input_text_color},
        {BrushId::SearchHighlight,  theme_.search_highlight_color},
        {BrushId::SearchHighlightCurrent, theme_.search_highlight_current_color},
        {BrushId::SearchNoMatchBg,  theme_.search_no_match_bg_color},
    };

    // 既存ブラシには SetColor のみ、未生成のものだけ CreateSolidColorBrush。
    // テーマ切替時の 40+ 個の COM オブジェクト再生成を回避する。
    for (const auto& s : specs) {
        auto& brush = brushes_[static_cast<size_t>(s.id)];
        if (brush) {
            brush->SetColor(s.color);
        }
        else {
            render_target_->CreateSolidColorBrush(s.color, &brush);
        }
    }
}

void Renderer::InvalidateBrushes() noexcept
{
    for (auto& b : brushes_) {
        b.Reset();
    }
}

ComPtr<IDWriteTextFormat> Renderer::CreatePaneFormat(
    const wchar_t* family, DWRITE_FONT_WEIGHT weight,
    float size, const wchar_t* locale)
{
    ComPtr<IDWriteTextFormat> fmt;
    backend_.GetDWriteFactory()->CreateTextFormat(
        family, nullptr, weight, DWRITE_FONT_STYLE_NORMAL,
        DWRITE_FONT_STRETCH_NORMAL, size, locale, &fmt);
    return fmt;
}

void Renderer::RecreatePaneFormats()
{
    // テーマサイズの更新に合わせて全ペイン/UIテキストフォーマットを再作成
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
        { &fmt_.icon_font,        body_font, W,                            theme_.font_size_body,         L"ja-jp", TA_LEAD, PA_TOP, false },
        { &fmt_.copy_btn_icon,    icon_font, W,                            theme_.font_size_body,         L"en-us", TA_CTR,  PA_CTR, true  },
        { &fmt_.list_number,      body_font, W,                            theme_.font_size_body,         L"ja-jp", TA_TAIL, PA_TOP, false },
        { &fmt_.placeholder_text, body_font, W,                            theme_.font_size_body,         L"ja-jp", TA_CTR,  PA_CTR, false },
        { &fmt_.titlebar_text,    body_font, W,                            theme_.pane_font_size,         L"ja-jp", TA_CTR,  PA_CTR, true  },
        { &fmt_.titlebar_icon,    icon_font, W,                            14.0f,                         L"en-us", TA_CTR,  PA_CTR, true  },
        { &fmt_.pane_icon,        icon_font, W,                            theme_.pane_font_size,         L"en-us", TA_CTR,  PA_CTR, true  },
        { &fmt_.pane_item,        body_font, W,                            theme_.pane_font_size,         L"ja-jp", TA_LEAD, PA_CTR, true  },
        { &fmt_.pane_header,      body_font, DWRITE_FONT_WEIGHT_SEMI_BOLD, theme_.pane_font_size,         L"ja-jp", TA_LEAD, PA_CTR, true  },
        { &fmt_.nav_button,       body_font, W,                            theme_.pane_font_size,         L"ja-jp", TA_CTR,  PA_CTR, true  },
        { &fmt_.gesture_overlay,  body_font, DWRITE_FONT_WEIGHT_BOLD,      32.0f * theme_.zoom,           L"ja-JP", TA_CTR,  PA_CTR, false },
        { &fmt_.toast_text,       body_font, DWRITE_FONT_WEIGHT_SEMI_BOLD, theme_.pane_font_size * 1.1f,  L"ja-JP", TA_CTR,  PA_CTR, true  },
        { &fmt_.search_input,     body_font, W,                            theme_.pane_font_size,         L"ja-jp", TA_LEAD, PA_CTR, true  },
        { &fmt_.search_count,     body_font, W,                            theme_.pane_font_size * 0.9f,  L"ja-jp", TA_CTR,  PA_CTR, true  },
        { &fmt_.search_icon,      icon_font, W,                            14.0f,                         L"en-us", TA_CTR,  PA_CTR, true  },
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

    nav_back_layout_.Reset();
    nav_forward_layout_.Reset();
    gesture_back_layout_.Reset();
    gesture_forward_layout_.Reset();
    cached_toast_layout_.Reset();
    cached_toast_text_.clear();
    cached_search_layout_.Reset();
    cached_search_text_.clear();
    cached_search_width_ = -1.0f;
    cached_search_height_ = -1.0f;
    cached_search_has_underline_ = false;
    if (fmt_.nav_button) {
        auto* dw = backend_.GetDWriteFactory();
        if (dw) {
            static const wchar_t BACK_ICON[] = L"\x25C0";
            static const wchar_t FORWARD_ICON[] = L"\x25B6";
            dw->CreateTextLayout(BACK_ICON, 1, fmt_.nav_button.Get(), NAV_BTN_SIZE, NAV_BTN_SIZE, &nav_back_layout_);
            dw->CreateTextLayout(FORWARD_ICON, 1, fmt_.nav_button.Get(), NAV_BTN_SIZE, NAV_BTN_SIZE, &nav_forward_layout_);
        }
    }
    if (fmt_.gesture_overlay) {
        auto* dw = backend_.GetDWriteFactory();
        if (dw) {
            static const wchar_t GESTURE_BACK[] = L"\x2190 \x623B\x308B";
            static const wchar_t GESTURE_FORWARD[] = L"\x2192 \x9032\x3080";
            dw->CreateTextLayout(GESTURE_BACK, 4, fmt_.gesture_overlay.Get(), 280.0f, 80.0f, &gesture_back_layout_);
            dw->CreateTextLayout(GESTURE_FORWARD, 4, fmt_.gesture_overlay.Get(), 280.0f, 80.0f, &gesture_forward_layout_);
        }
    }

    // ペインキャッシュを無効化して新しいサイズで再描画させる
    file_pane_cache_.Reset();
    toc_pane_cache_.Reset();

    // コマンドジェネレータのフォーマットを更新
    cmd_generator_.SetFormats({ fmt_.list_number.Get(), fmt_.icon_font.Get(), fmt_.copy_btn_icon.Get(), fmt_.placeholder_text.Get() });
}
