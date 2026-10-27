#include "renderer.h"
#include "d2d_util.h"
#include "i18n.h"
#include "resource.h"
#include "ui_constants.h"
#include "utility.h"
#include "wic_util.h"
#include "win32_util.h"
#include "profiler.h"
#include <d2d1_1.h>
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

    auto* wic = wic_factory_.Get();
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

    // 既存ブラシには SetColor のみ、未生成のものだけ CreateSolidColorBrush。
    // テーマ切替時の 50 個ほどの COM オブジェクト再生成を回避する。
    for (size_t i = 0; i < brushes_.size(); ++i) {
        auto& brush = brushes_[i];
        if (brush) {
            brush->SetColor(theme_->colors[i]);
        }
        else {
            CreateSolidColorBrushOrFallback(target, theme_->colors[i], brush);
        }
    }
}

void Renderer::EnsurePaneListFormats(PaneTarget target, float zoom)
{
    auto& f = pane_list_fmt_[std::to_underlying(target)];
    if (f.zoom == zoom) {
        return;
    }
    auto* const dw = dwrite_factory_.Get();
    const float size = theme_->pane_font_size * zoom;
    f.item = CreateTextFormat(dw, { .family = theme_->font_family.c_str(), .size = size, .locale = theme_->text_locale, .para_align = DWRITE_PARAGRAPH_ALIGNMENT_CENTER, .no_wrap = true });
    // アイコンはファイル一覧だけ。
    if (target == PaneTarget::File) {
        f.icon = CreateTextFormat(dw, { .family = theme_->icon_font.c_str(), .size = size, .text_align = DWRITE_TEXT_ALIGNMENT_CENTER, .para_align = DWRITE_PARAGRAPH_ALIGNMENT_CENTER, .no_wrap = true });
    }
    f.zoom = zoom;
}

void Renderer::RecreateAllTextFormats()
{
    CreateTextFormats();

    for (auto& f : pane_list_fmt_) {
        f = {};
    }

    // 古い書式のレイアウトは二度と引かれないので、まとめて捨てる。
    text_layouts_.SetFactory(dwrite_factory_.Get());
    search_input_.SetFormat(dwrite_factory_.Get(), fmt_.search_input.Get());
}

void Renderer::CreateTextFormats(bool zoomed_only)
{
    constexpr DWRITE_FONT_WEIGHT W = DWRITE_FONT_WEIGHT_NORMAL;
    constexpr DWRITE_FONT_WEIGHT SEMI = DWRITE_FONT_WEIGHT_SEMI_BOLD;
    constexpr DWRITE_TEXT_ALIGNMENT TA_LEAD = DWRITE_TEXT_ALIGNMENT_LEADING;
    constexpr DWRITE_TEXT_ALIGNMENT TA_CTR = DWRITE_TEXT_ALIGNMENT_CENTER;
    constexpr DWRITE_TEXT_ALIGNMENT TA_TAIL = DWRITE_TEXT_ALIGNMENT_TRAILING;
    constexpr DWRITE_PARAGRAPH_ALIGNMENT PA_TOP = DWRITE_PARAGRAPH_ALIGNMENT_NEAR;
    constexpr DWRITE_PARAGRAPH_ALIGNMENT PA_CTR = DWRITE_PARAGRAPH_ALIGNMENT_CENTER;

    const wchar_t* const body_font = theme_->font_family.c_str();
    const wchar_t* const icon_font = theme_->icon_font.c_str();

    // i18n::TextLocale の文書から来る文字は Doc、UI の決まった文言は Ui のロケールで組む。アイコンは En。
    enum class Loc : uint8_t {
        Doc,
        Ui,
        En,
    };
    struct FormatSpec {
        // ComPtr の & は中身を解放するので、参照で持つ。
        ComPtr<IDWriteTextFormat>& target;
        Loc loc;
        // locale は loc から決める。
        TextFormatSpec spec;
        // 大きさがズームに従う。
        bool zoomed = false;
    };

    // clang-format off
    const FormatSpec specs[] = {
        { fmt_.md.checkbox,          Loc::Doc,  { body_font, W,                       theme_->font_size_body,        nullptr, TA_LEAD, PA_TOP, false, BODY_LINE_HEIGHT_FACTOR }, true },
        { fmt_.md.copy_btn_icon,     Loc::En,   { icon_font, W,                       theme_->font_size_body,        nullptr, TA_CTR,  PA_CTR, true  }, true },
        { fmt_.md.list_number,       Loc::Doc,  { body_font, W,                       theme_->font_size_body,        nullptr, TA_TAIL, PA_TOP, true,  BODY_LINE_HEIGHT_FACTOR }, true },
        { fmt_.md.placeholder_text,  Loc::Ui,   { body_font, W,                       theme_->font_size_body,        nullptr, TA_CTR,  PA_CTR, false }, true },
        { fmt_.titlebar_text,        Loc::Doc,  { body_font, W,                       TITLEBAR_TEXT_FONT_SIZE,       nullptr, TA_CTR,  PA_CTR, true  } },
        { fmt_.ui_icon,              Loc::En,   { icon_font, W,                       UI_ICON_FONT_SIZE,             nullptr, TA_CTR,  PA_CTR, true  } },
        { fmt_.pane_icon,            Loc::En,   { icon_font, W,                       theme_->pane_font_size,        nullptr, TA_CTR,  PA_CTR, true  } },
        { fmt_.pane_header,          Loc::Ui,   { body_font, SEMI,                    theme_->pane_font_size,        nullptr, TA_LEAD, PA_CTR, true  } },
        { fmt_.nav_button,           Loc::Doc,  { body_font, W,                       theme_->pane_font_size,        nullptr, TA_CTR,  PA_CTR, true  } },
        { fmt_.gesture_overlay,      Loc::Ui,   { body_font, DWRITE_FONT_WEIGHT_BOLD, 32.0f * theme_->zoom,          nullptr, TA_CTR,  PA_CTR, false }, true },
        { fmt_.toast_text,           Loc::Ui,   { body_font, SEMI,                    theme_->pane_font_size * 1.1f, nullptr, TA_CTR,  PA_CTR, true  } },
        { fmt_.search_input,         Loc::Doc,  { body_font, W,                       SEARCH_INPUT_FONT_SIZE,        nullptr, TA_LEAD, PA_CTR, true  } },
        { fmt_.search_count,         Loc::Doc,  { body_font, W,                       SEARCH_COUNT_FONT_SIZE,        nullptr, TA_CTR,  PA_CTR, true  } },
    };
    // clang-format on

    const auto locale_of = [this](Loc loc) noexcept {
        switch (loc) {
        case Loc::Doc:
            return theme_->text_locale;
        case Loc::Ui:
            return i18n::S().lang_locale;
        case Loc::En:
            break;
        }
        return L"en-us";
    };
    auto* const dw = dwrite_factory_.Get();
    for (const auto& s : specs) {
        if (zoomed_only && !s.zoomed) {
            continue;
        }
        auto spec = s.spec;
        spec.locale = locale_of(s.loc);
        s.target = CreateTextFormat(dw, spec);
    }
}
