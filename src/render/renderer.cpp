#include "renderer.h"
#include "syntax.h"
#include "resource.h"
#include "ui_constants.h"
#include "profiler.h"
#include <algorithm>
#include <cmath>

using Microsoft::WRL::ComPtr;

#pragma comment(lib, "d2d1.lib")
#pragma comment(lib, "dwrite.lib")

bool Renderer::Init(HWND hwnd)
{
    theme_ = GetLightTheme();

    if (!backend_.Init(hwnd)) {
        return false;
    }

    RecreateBrushes();
    RecreatePaneFormats();
    LoadAppIconBitmap();

    // 滑らかなジェスチャー軌跡のための丸型キャップと結合
    const D2D1_STROKE_STYLE_PROPERTIES ssp = D2D1::StrokeStyleProperties(
        D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND,
        D2D1_CAP_STYLE_ROUND, D2D1_LINE_JOIN_ROUND);
    backend_.GetD2DFactory()->CreateStrokeStyle(ssp, nullptr, 0, &gesture_stroke_style_);

    measurer_.SetFactory(backend_.GetDWriteFactory());
    if (!layout_.Init(&measurer_, theme_)) {
        return false;
    }

    cmd_generator_.SetTheme(&theme_);
    cmd_generator_.SetFormats({ fmt_.list_number.Get(), fmt_.icon_font.Get(), fmt_.copy_btn_icon.Get(), fmt_.placeholder_text.Get() });
    cmd_generator_.SetSharedHitTestBuffer(&hit_test_buffer_);

    return true;
}

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

    ComPtr<IWICFormatConverter> converter;
    hr = wic->CreateFormatConverter(&converter);
    if (FAILED(hr)) {
        return;
    }

    hr = converter->Initialize(wic_bitmap.Get(), GUID_WICPixelFormat32bppPBGRA,
        WICBitmapDitherTypeNone, nullptr, 0.0, WICBitmapPaletteTypeMedianCut);
    if (FAILED(hr)) {
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

    for (auto& b : brushes_) {
        b.Reset();
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

    for (const auto& s : specs) {
        render_target_->CreateSolidColorBrush(s.color, &brushes_[static_cast<size_t>(s.id)]);
    }
}

void Renderer::SetTheme(const Theme& theme)
{
    theme_ = theme;
    UpdateLayoutTheme();
    RecreatePaneFormats();
    cmd_generator_.SetTheme(&theme_);
    if (!backend_.GetRenderTarget()) {
        return;
    }
    RecreateBrushes();
}

void Renderer::Resize(UINT width, UINT height) noexcept
{
    backend_.Resize(width, height);
}

void Renderer::SetDpi(float dpi) noexcept
{
    backend_.SetDpi(dpi);
    // 新しいDPIでペインキャッシュを再作成
    file_pane_cache_.Reset();
    toc_pane_cache_.Reset();
}

void Renderer::ApplyZoom(float new_zoom)
{
    theme_.ApplyZoom(new_zoom);
    UpdateLayoutTheme();
    RecreatePaneFormats();
}

void Renderer::ApplyZoomFromBase(const Theme& base_theme, float new_zoom)
{
    theme_ = base_theme;
    if (new_zoom != 1.0f) {
        theme_.ApplyZoom(new_zoom);
    }
    UpdateLayoutTheme();
    RecreatePaneFormats();
    cmd_generator_.SetTheme(&theme_);
}

void Renderer::UpdateLayoutTheme()
{
    layout_.UpdateTheme(theme_);
    layout_.RecreateFormats();
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

// ---- ノード描画 ----
// ノード描画ロジックはCommandGeneratorに抽出済み。
// D2Dブラシが必要なApplyNodeEffectsのみ描画前パスとしてここに残る。

void Renderer::PrepareVisibleEffects(std::pmr::vector<Node>& nodes, LayoutCache& cache,
    float scroll_y, float md_pane_height)
{
    const float viewport_top = scroll_y;
    const float viewport_bottom = scroll_y + md_pane_height;
    const int first_visible = FindFirstVisibleNodeIndex(cache, nodes.size(), viewport_top);

    const uint32_t effects_gen = cache.GetEffectsGeneration();
    if (effects_gen != last_effects_gen_ || first_visible != last_effects_first_
        || std::abs(viewport_bottom - last_effects_bottom_) > 0.5f) {
        MENDO_PROFILE("PrepareVisibleEffects");
        ApplyVisibleEffects(nodes, cache, first_visible, viewport_top, viewport_bottom);
        last_effects_gen_ = effects_gen;
        last_effects_first_ = first_visible;
        last_effects_bottom_ = viewport_bottom;
    }
}

void Renderer::ApplyVisibleEffects(std::pmr::vector<Node>& nodes, LayoutCache& cache,
    int first_visible, float viewport_top, float viewport_bottom)
{
    const int node_count = static_cast<int>(nodes.size());
    for (int i = first_visible; i < node_count; i++) {
        if (cache[i].y_position > viewport_bottom) {
            break;
        }
        ApplyNodeEffects(nodes[i], cache[i], viewport_top, viewport_bottom);
    }
}

ID2D1SolidColorBrush* Renderer::GetSyntaxBrush(SyntaxTokenType type) const noexcept
{
    static constexpr BrushId SYNTAX_MAP[] = {
        BrushId::Text,                // Plain（未使用、フォールバックとしてテキストブラシを返す）
        BrushId::SyntaxKeyword,       // キーワード
        BrushId::SyntaxType,          // 型
        BrushId::SyntaxString,        // 文字列
        BrushId::SyntaxNumber,        // 数値
        BrushId::SyntaxComment,       // コメント
        BrushId::SyntaxPreprocessor,  // プリプロセッサ
        BrushId::SyntaxFunction,      // 関数
    };
    const auto idx = static_cast<size_t>(type);
    if (idx >= std::size(SYNTAX_MAP)) {
        return nullptr;
    }
    return Brush(SYNTAX_MAP[idx]);
}

// DWRITE_HIT_TEST_METRICS からパディング適用済みの InlineCodeBg を生成する。
static InlineCodeBg MakeInlineCodeBg(const DWRITE_HIT_TEST_METRICS& m) noexcept
{
    return D2D1::RectF(
        m.left - INLINE_CODE_PAD_X,
        m.top - INLINE_CODE_PAD_Y,
        m.left + m.width + INLINE_CODE_PAD_X,
        m.top + m.height + INLINE_CODE_PAD_Y
    );
}

void Renderer::ApplyTableEffects(Node& node, NodeLayoutEntry& entry,
    float viewport_top, float viewport_bottom)
{
    if (!node.has_table() || node.table_rows().empty() || !entry.has_table_layout()) {
        entry.effects_applied = true;
        return;
    }

    const bool first_pass = !entry.effects_applied;
    const auto& rows = node.table_rows();
    const auto row_count = rows.size();
    auto& tl = *entry.table_layout;
    if (first_pass) {
        entry.effects_applied = true;
        tl.cell_inline_code_bgs.resize(row_count * tl.col_count);
        tl.row_bgs_computed.resize(row_count, 0);
    }

    const float border = TABLE_BORDER_WIDTH;
    float row_y = entry.y_position;

    for (size_t r = 0; r < row_count; r++) {
        const auto& row = rows[r];
        const float row_h = (r < tl.row_heights.size()) ? tl.row_heights[r] : (theme_.font_size_body * 1.4f);
        const float row_bottom = row_y + row_h + border;

        const bool row_visible = (viewport_top < 0.0f) || (row_bottom >= viewport_top && row_y <= viewport_bottom);
        // 2回目以降のパスではオフスクリーン行の背景走査をスキップ
        if (!first_pass && !row_visible) {
            row_y = row_bottom;
            continue;
        }
        // row_bgs_computed フラグで O(1) 判定（O(cells × runs) の走査を排除）
        const bool bgs_done = !first_pass && r < tl.row_bgs_computed.size() && tl.row_bgs_computed[r];
        const bool need_bgs = row_visible && !bgs_done;

        // 2回目以降: インラインコード背景の計算が不要な行はスキップ
        if (!first_pass && !need_bgs) {
            row_y = row_bottom;
            continue;
        }

        const auto col_count = row.cells.size();
        for (size_t c = 0; c < col_count; c++) {
            IDWriteTextLayout* cell_layout = tl.GetCellLayout(r, c);
            if (!cell_layout) {
                continue;
            }
            for (const auto& run : row.cells[c].runs) {
                // リンク色: 初回パスで全行に適用（軽量・冪等）
                if (first_pass && run.has_link()) {
                    const DWRITE_TEXT_RANGE range{ run.start, run.length };
                    cell_layout->SetDrawingEffect(Brush(BrushId::Link), range);
                }
                // インラインコード背景: 可視かつ未計算の行のみ
                if (need_bgs && run.code() && run.length > 0) {
                    const UINT32 count = FetchHitTestMetrics(cell_layout, run.start, run.length, hit_test_buffer_);
                    for (UINT32 hi = 0; hi < count; hi++) {
                        tl.cell_inline_code_bgs[tl.CellIndex(r, c)].emplace_back(MakeInlineCodeBg(hit_test_buffer_[hi]));
                    }
                }
            }
        }
        // この行のインラインコード背景計算が完了したことを記録
        if (need_bgs && r < tl.row_bgs_computed.size()) {
            tl.row_bgs_computed[r] = true;
        }
        row_y = row_bottom;
    }
}

void Renderer::ApplyNodeEffects(Node& node, NodeLayoutEntry& entry,
    float viewport_top, float viewport_bottom)
{
    // テーブルノード: ビューポートカリング付きの増分処理を行う。
    // リンク色は全行に適用（軽量・冪等）、インラインコード背景は可視行のみ計算する。
    if (node.type == NodeType::Table) {
        ApplyTableEffects(node, entry, viewport_top, viewport_bottom);
        return;
    }

    if (entry.effects_applied) {
        return;
    }
    entry.effects_applied = true;

    // 画像ノード: テキストエフェクト不要
    if (node.type == NodeType::Image) {
        return;
    }

    if (!entry.text_layout) {
        return;
    }

    // コードブロックにシンタックスハイライトを適用
    // トークン化はMeasureNode（レイアウトパス）で事前実行済み
    if (node.type == NodeType::CodeBlock) {
        for (const auto& token : node.syntax_tokens()) {
            if (token.type == SyntaxTokenType::Plain) {
                continue;
            }
            auto* brush = GetSyntaxBrush(token.type);
            if (brush) {
                const DWRITE_TEXT_RANGE range{ token.start, token.length };
                entry.text_layout->SetDrawingEffect(brush, range);
            }
        }
    }

    // Alertラベルの色を適用
    if (node.type == NodeType::BlockQuote && node.alert_type != AlertType::None && node.alert_label_length > 0) {
        static constexpr BrushId ALERT_BRUSH[] = {
            BrushId::AlertNote, BrushId::AlertTip, BrushId::AlertImportant,
            BrushId::AlertWarning, BrushId::AlertCaution,
        };
        static_assert(std::size(ALERT_BRUSH) == ALERT_TYPE_COUNT);
        const auto idx = AlertColorIndex(node.alert_type);
        if (idx < ALERT_TYPE_COUNT) {
            const DWRITE_TEXT_RANGE range{ 0, node.alert_label_length };
            entry.text_layout->SetDrawingEffect(Brush(ALERT_BRUSH[idx]), range);
        }
    }

    // リンクの下線/色を適用し、インラインコード背景の矩形をキャッシュ
    for (const auto& run : node.runs) {
        if (run.has_link()) {
            const DWRITE_TEXT_RANGE range{ run.start, run.length };
            entry.text_layout->SetUnderline(TRUE, range);
            entry.text_layout->SetDrawingEffect(Brush(BrushId::Link), range);
        }
        if (run.code() && node.type != NodeType::CodeBlock && run.length > 0) {
            const UINT32 count = FetchHitTestMetrics(entry.text_layout.Get(), run.start, run.length, hit_test_buffer_);
            for (UINT32 i = 0; i < count; i++) {
                entry.inline_code_bgs.emplace_back(MakeInlineCodeBg(hit_test_buffer_[i]));
            }
        }
    }
}

// ---- メイン描画 ----

void Renderer::DrawSidePanes(const SidePaneState& sp)
{
    if (sp.show_file_pane) {
        DrawFileExplorer(sp.file_entries, sp.file_pane_rect, sp.file_scroll, sp.hovered_file_index, sp.file_close_hovered, sp.file_refresh_hovered);
        DrawSplitter(sp.file_pane_rect.x + sp.file_pane_rect.width, sp.file_pane_rect.y, sp.file_pane_rect.y + sp.file_pane_rect.height);
    }
    if (sp.show_toc_pane) {
        DrawToc(sp.toc_entries, sp.nodes, sp.toc_pane_rect, sp.toc_scroll, sp.hovered_toc_index, sp.toc_close_hovered, sp.active_toc_index);
        DrawSplitter(sp.toc_pane_rect.x + sp.toc_pane_rect.width, sp.toc_pane_rect.y, sp.toc_pane_rect.y + sp.toc_pane_rect.height);
    }
}

void Renderer::DrawLoading(float angle,
    const PaneRect& md_pane_rect,
    const SidePaneState& sp,
    const TitleBarRenderState& titlebar,
    const GestureRenderState& gesture,
    const ToastRenderState& toast)
{
    if (!rt()) {
        return;
    }

    rt()->BeginDraw();
    rt()->Clear(theme_.bg_color);

    // カスタムタイトルバーを描画
    DrawTitleBar(titlebar);

    DrawSidePanes(sp);

    // MDペイン中央にスピナーを描画
    const float cx = md_pane_rect.x + md_pane_rect.width / 2.0f;
    const float cy = md_pane_rect.y + md_pane_rect.height / 2.0f;
    for (int i = 0; i < spinner::DOT_COUNT; i++) {
        const float a = angle - i * (TWO_PI / spinner::DOT_COUNT);
        const float dx = cx + spinner::RADIUS * std::cos(a);
        const float dy = cy + spinner::RADIUS * std::sin(a);
        const float alpha = 1.0f - i * (spinner::DOT_FADE_FACTOR / spinner::DOT_COUNT);

        const D2D1_ELLIPSE ellipse = D2D1::Ellipse(D2D1::Point2F(dx, dy), spinner::DOT_RADIUS, spinner::DOT_RADIUS);
        Brush(BrushId::Text)->SetOpacity(alpha);
        rt()->FillEllipse(ellipse, Brush(BrushId::Text));
    }
    Brush(BrushId::Text)->SetOpacity(1.0f);

    // ジェスチャーオーバーレイ（ローディング中でもフェードアウト中は表示）
    if (gesture.overlay_visible && gesture.overlay_alpha > 0.0f) {
        DrawGestureOverlay(gesture.direction, gesture.overlay_alpha, md_pane_rect);
    }

    // トースト通知
    if (toast.visible) {
        DrawToastOverlay(toast, md_pane_rect);
    }

    if (!CheckEndDraw()) {
        return;
    }
}

void Renderer::Render(const RenderParams& p)
{
    if (!rt()) {
        return;
    }

    rt()->BeginDraw();
    rt()->Clear(theme_.bg_color);

    // カスタムタイトルバーを描画
    DrawTitleBar(p.titlebar);

    DrawSidePanes(p.side_panes);

    // 最初の可視ノードを検索（ヒットテストとの座標一致のためスナップ前の scroll_y を使う）。
    const float viewport_top = p.scroll_y;
    const int first_visible = FindFirstVisibleNodeIndex(p.cache, p.nodes.size(), viewport_top);

    // Markdownコンテンツペインの描画コマンドを生成・実行。
    const float dpi_scale = backend_.GetDpi() / DEFAULT_DPI;
    {
        MENDO_PROFILE("GenerateMdPane");
        const auto& cmds = cmd_generator_.GenerateMdPane(p.nodes, p.cache, p.md_pane_rect, p.scroll_y, p.selection, first_visible, p.hovered_copy_node, p.hovered_save_node, dpi_scale);
        {
            MENDO_PROFILE("CommandExecutor::Execute");
            cmd_executor_.Execute(cmds, rt());
        }
    }

    // ナビゲーションオーバーレイボタン（戻る/進む）を描画
    if (p.can_go_back || p.can_go_forward) {
        DrawNavOverlay(p.md_pane_rect, p.can_go_back, p.can_go_forward, p.nav_hovered);
    }

    // ジェスチャー軌跡
    if (p.gesture.trail_active && p.gesture.trail_points && p.gesture.trail_points->size() >= 2) {
        DrawGestureTrail(*p.gesture.trail_points);
    }

    // ジェスチャーオーバーレイ（アクション後のフェードアウト）
    if (p.gesture.overlay_visible && p.gesture.overlay_alpha > 0.0f) {
        DrawGestureOverlay(p.gesture.direction, p.gesture.overlay_alpha, p.md_pane_rect);
    }

    // トースト通知
    if (p.toast.visible) {
        DrawToastOverlay(p.toast, p.md_pane_rect);
    }

    // 検索バー
    if (p.search_bar.visible) {
        DrawSearchBar(p.search_bar, p.md_pane_rect);
    }

    // Markdownペインのカスタムスクロールバー
    DrawMdScrollbar(p.md_pane_rect, p.scroll_y, p.total_content_height, p.has_dirty_nodes);

    if (!CheckEndDraw()) {
        return;
    }
}

bool Renderer::CheckEndDraw()
{
    const HRESULT hr = rt()->EndDraw();
    if (hr == D2DERR_RECREATE_TARGET) {
        RecreateRenderTarget();
        // 現在のフレームは破棄された — 新しいターゲットで再描画を要求
        InvalidateRect(backend_.GetHwnd(), nullptr, FALSE);
        return false;
    }
    if (SUCCEEDED(hr)) {
        backend_.Present();
    }
    return SUCCEEDED(hr);
}

bool Renderer::RecreateRenderTarget()
{
    if (!backend_.RecreateRenderTarget()) {
        return false;
    }

    RecreateBrushes();
    LoadAppIconBitmap();
    file_pane_cache_.Reset();
    toc_pane_cache_.Reset();
    cmd_executor_ = CommandExecutor{}; // バインドされたレンダーターゲットをリセット

    // 依存リソース（例: MermaidRendererのビットマップ）が更新されるようオーナーに通知
    if (on_device_lost_) {
        on_device_lost_(backend_.GetRenderTarget());
    }

    return true;
}

