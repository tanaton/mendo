#include "renderer.h"
#include "d2d_util.h"
#include "i18n.h"
#include "profiler.h"
#include "resource.h"
#include "ui_constants.h"
#include "utility.h"
#include "wic_util.h"
#include "win32_util.h"
#include <d2d1_1.h>
#include <wincodec.h>
#include <utility>
#include <wrl/client.h>

using Microsoft::WRL::ComPtr;

ID2D1RenderTarget* Renderer::GetRenderTarget() const noexcept
{
    return device_context_.Get();
}

bool Renderer::Init(HWND hwnd, const Theme& theme)
{
    theme_ = &theme;

    if (!CreateFactories(hwnd)) {
        return false;
    }

    RecreateAllTextFormats();
    DecodeAppIcon();

    cmd_generator_.Init(theme, &fmt_.md, &text_layouts_);

    return true;
}

void Renderer::TrimForMinimize() noexcept
{
    TrimDevice();
    toc_titles_ = {};
}

void Renderer::PrepareChrome(const TitleBarRenderState& titlebar, const SidePaneState& sp)
{
    EnsurePaneListFormats(PaneTarget::File, sp.Get(PaneTarget::File).zoom);
    EnsurePaneListFormats(PaneTarget::Toc, sp.Get(PaneTarget::Toc).zoom);
    frame_cmds_.clear();
    chrome::EmitTitleBar(frame_cmds_, text_layouts_, fmt_.ui_icon.Get(), fmt_.titlebar_text.Get(), app_icon_bitmap_.Get(), theme_->dark, titlebar);
    const auto& file_fmt = pane_list_fmt_[std::to_underlying(PaneTarget::File)];
    const auto& toc_fmt = pane_list_fmt_[std::to_underlying(PaneTarget::Toc)];
    const chrome::SidePaneFormats fmts{
        .header = fmt_.pane_header.Get(),
        .header_icon = fmt_.pane_icon.Get(),
        .item = { file_fmt.item.Get(), toc_fmt.item.Get() },
        .file_icon = file_fmt.icon.Get(),
    };
    chrome::EmitSidePanes(frame_cmds_, text_layouts_, fmts, *theme_, sp, toc_titles_);
}

std::optional<Renderer::FrameResult> Renderer::BeginFrame(const TitleBarRenderState& titlebar, const SidePaneState& side_panes)
{
    if (device_lost_) {
        return RecreateDeviceForFrame();
    }
    if (!rt()) {
        return FrameResult::Done;
    }

    text_layouts_.BeginFrame();
    PrepareChrome(titlebar, side_panes);
    return std::nullopt;
}

Renderer::FrameResult Renderer::Render(const RenderParams& p)
{
    MENDO_PROFILE("Render");

    if (const auto early = BeginFrame(p.titlebar, p.side_panes)) {
        return *early;
    }

    if (!p.loading_angle) {
        cmd_generator_.GenerateMdPane(frame_cmds_, p.md, DpiScaleFrom(dpi_));
        if (p.md.inline_math) {
            ++p.md.inline_math->frame;
        }
    }
    GenOverlays(p);
    MENDO_PLOT("draw.command_count", static_cast<int64_t>(frame_cmds_.size()));

    // メッセージループが取っていなければ (モーダルループや UpdateWindow からの描画) ここで待つ。
    AcquireFrame();
    rt()->BeginDraw();
    rt()->Clear(theme_->Color(BrushId::Background));
    ExecuteDrawCommands(frame_cmds_, rt(), brushes_, p.md.inline_math);

    return EndFrame();
}

void Renderer::GenOverlays(const RenderParams& p)
{
    gesture_trail_.Reset();
    const PaneRect& md_pane_rect = p.md.md_pane_rect;
    const bool dark = theme_->dark;
    if (p.loading_angle) {
        overlay::EmitLoadingSpinner(frame_cmds_, *p.loading_angle, md_pane_rect);
    }
    else {
        if (p.can_go_back || p.can_go_forward) {
            overlay::EmitNavButtons(frame_cmds_, text_layouts_, fmt_.nav_button.Get(), md_pane_rect, dark, p.can_go_back, p.can_go_forward, p.nav_hovered);
        }
        if (p.gesture.trail) {
            GenGestureTrail(*p.gesture.trail);
        }
    }
    // 読み込み中もジェスチャーとトーストは出す。
    overlay::EmitGestureOverlay(frame_cmds_, text_layouts_, fmt_.gesture_overlay.Get(), md_pane_rect, dark, p.gesture.direction);
    overlay::EmitToast(frame_cmds_, text_layouts_, fmt_.toast_text.Get(), md_pane_rect, dark, p.toast_message);
    if (p.loading_angle) {
        return;
    }
    if (p.search_bar.visible) {
        const auto sbl = ComputeSearchBarLayout(md_pane_rect, !p.search_bar.query.empty());
        const auto input = search_input_.Measure(p.search_bar, sbl.text_width());
        overlay::EmitSearchBar(frame_cmds_, text_layouts_, fmt_.ui_icon.Get(), fmt_.search_count.Get(), sbl, p.search_bar, input);
    }
    if (const auto thumb = VScrollThumb(md_pane_rect, 0.0f, p.md.scroll_y, ComputeTotalContentHeight(p.md.cache, theme_->margin_bottom), p.md.cache.HasDirtyNodes())) {
        frame_cmds_.push_back(*thumb);
    }
}

Renderer::FrameResult Renderer::EndFrame()
{
    const HRESULT hr = rt()->EndDraw();
    if (hr == D2DERR_RECREATE_TARGET) {
        return RecreateDeviceForFrame();
    }
    if (SUCCEEDED(hr)) {
        Present();
        // REMOVED/RESET/HUNG/DRIVER_INTERNAL_ERROR で device_lost_ をセット済み。
        // 次のフレームの BeginFrame で作り直す。
        if (device_lost_) {
            return FrameResult::RedrawNow;
        }
    }
    return FrameResult::Done;
}

bool Renderer::RecreateDevice()
{
    if (!CreateDevice()) {
        return false;
    }
    if (on_device_lost_) {
        on_device_lost_(rt());
    }
    return true;
}

void Renderer::GenGestureTrail(const std::vector<DipPoint>& points)
{
    auto* const factory = GetD2DFactory();
    if (points.size() < 2 || !factory) {
        return;
    }
    // パスジオメトリで一筆描きすることで、結合部のアルファ蓄積（節）を防ぐ
    ComPtr<ID2D1GeometrySink> sink;
    if (FAILED(factory->CreatePathGeometry(&gesture_trail_)) || FAILED(gesture_trail_->Open(&sink))) {
        gesture_trail_.Reset();
        return;
    }
    sink->BeginFigure(D2D1::Point2F(points[0].x, points[0].y), D2D1_FIGURE_BEGIN_HOLLOW);
    for (size_t i = 1; i < points.size(); i++) {
        sink->AddLine(D2D1::Point2F(points[i].x, points[i].y));
    }
    sink->EndFigure(D2D1_FIGURE_END_OPEN);
    if (FAILED(sink->Close())) {
        gesture_trail_.Reset();
        return;
    }

    if (!gesture_stroke_style_) {
        // 滑らかなジェスチャー軌跡のための丸型キャップと結合（初回のみ生成）
        const D2D1_STROKE_STYLE_PROPERTIES ssp = D2D1::StrokeStyleProperties(D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND, D2D1_LINE_JOIN_ROUND);
        factory->CreateStrokeStyle(ssp, nullptr, 0, &gesture_stroke_style_);
    }
    frame_cmds_.push_back(DrawGeometryCmd{
        .geometry = gesture_trail_.Get(),
        .stroke_style = gesture_stroke_style_.Get(),
        .stroke_width = GESTURE_TRAIL_STROKE_WIDTH,
        .brush_id = BrushId::GestureTrail,
        .opacity = 0.5f,
    });
}

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
        { fmt_.md.placeholder_text,  Loc::Ui,   { body_font, W,                       theme_->font_size_body,        nullptr, TA_CTR,  PA_CTR, false, 0.0f, true }, true },
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
