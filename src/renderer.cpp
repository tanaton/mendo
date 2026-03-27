#include "renderer.h"
#include "ui_constants.h"
#include <algorithm>
#include <cmath>

#pragma comment(lib, "d2d1.lib")
#pragma comment(lib, "dwrite.lib")

bool Renderer::Init(HWND hwnd) {
    theme_ = GetLightTheme();

    if (!backend_.Init(hwnd)) {
        return false;
    }

    RecreateBrushes();
    RecreatePaneFormats();

    // 滑らかなジェスチャー軌跡のための丸型キャップと結合
    D2D1_STROKE_STYLE_PROPERTIES ssp = D2D1::StrokeStyleProperties(
        D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND,
        D2D1_CAP_STYLE_ROUND, D2D1_LINE_JOIN_ROUND);
    backend_.GetD2DFactory()->CreateStrokeStyle(ssp, nullptr, 0, &gesture_stroke_style_);

    measurer_.SetFactory(backend_.GetDWriteFactory());
    if (!layout_.Init(&measurer_, theme_)) {
        return false;
    }

    cmd_generator_.SetTheme(&theme_);
    cmd_generator_.SetFormats({ fmt_.list_number.Get(), fmt_.icon_font.Get(), fmt_.copy_btn_icon.Get() });

    return true;
}

void Renderer::RecreateBrushes() {
    auto* render_target_ = backend_.GetRenderTarget();
    if (!render_target_) {
        return;
    }

    for (auto& b : brushes_) {
        b.Reset();
    }

    bool is_dark = theme_.IsDark();

    float stripe_alpha = is_dark ? 0.05f : 0.02f;
    float thumb_alpha = is_dark ? 0.4f : 0.25f;

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
    };

    for (const auto& s : specs) {
        render_target_->CreateSolidColorBrush(s.color, &brushes_[static_cast<size_t>(s.id)]);
    }
}

void Renderer::SetTheme(const Theme& theme) {
    theme_ = theme;
    UpdateLayoutTheme();
    RecreatePaneFormats();
    cmd_generator_.SetTheme(&theme_);
    if (!backend_.GetRenderTarget()) {
        return;
    }
    RecreateBrushes();
}

void Renderer::Resize(UINT width, UINT height) {
    backend_.Resize(width, height);
}

void Renderer::SetDpi(float dpi) {
    backend_.SetDpi(dpi);
    // 新しいDPIでペインキャッシュを再作成
    file_pane_cache_.Reset();
    toc_pane_cache_.Reset();
}

void Renderer::ApplyZoom(float new_zoom) {
    theme_.ApplyZoom(new_zoom);
    UpdateLayoutTheme();
    RecreatePaneFormats();
}

void Renderer::ApplyZoomFromBase(const Theme& base_theme, float new_zoom) {
    theme_ = base_theme;
    if (new_zoom != 1.0f) {
        theme_.ApplyZoom(new_zoom);
    }
    UpdateLayoutTheme();
    RecreatePaneFormats();
    cmd_generator_.SetTheme(&theme_);
}

void Renderer::UpdateLayoutTheme() {
    layout_.UpdateTheme(theme_);
    layout_.RecreateFormats();
}

void Renderer::LayoutAllNodes(std::pmr::vector<Node>& nodes, LayoutCache& cache, float viewport_width) {
    float content_width = std::max(0.0f, viewport_width - theme_.margin_left - theme_.margin_right);
    layout_.LayoutNodes(nodes, cache, content_width);
}

ComPtr<IDWriteTextFormat> Renderer::CreatePaneFormat(
    const wchar_t* family, DWRITE_FONT_WEIGHT weight,
    float size, const wchar_t* locale) {
    ComPtr<IDWriteTextFormat> fmt;
    backend_.GetDWriteFactory()->CreateTextFormat(
        family, nullptr, weight, DWRITE_FONT_STYLE_NORMAL,
        DWRITE_FONT_STRETCH_NORMAL, size, locale, &fmt);
    return fmt;
}

void Renderer::RecreatePaneFormats() {
    // テーマサイズの更新に合わせて全ペイン/UIテキストフォーマットを再作成
    auto W = DWRITE_FONT_WEIGHT_NORMAL;

    fmt_.icon_font = CreatePaneFormat(L"Segoe Fluent Icons", W, theme_.font_size_body, L"en-us");

    // コピーボタン用アイコンフォーマット（両軸中央揃え）
    fmt_.copy_btn_icon = CreatePaneFormat(L"Segoe Fluent Icons", W, theme_.font_size_body, L"en-us");
    if (fmt_.copy_btn_icon) {
        fmt_.copy_btn_icon->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        fmt_.copy_btn_icon->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
        fmt_.copy_btn_icon->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    }

    // リスト番号フォーマット（順序付きリストの番号を右揃え）
    fmt_.list_number = CreatePaneFormat(theme_.font_family.c_str(), W, theme_.font_size_body, L"ja-jp");
    if (fmt_.list_number) {
        fmt_.list_number->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_TRAILING);
    }

    // タイトルバー用テキストフォーマット
    fmt_.titlebar_text = CreatePaneFormat(theme_.font_family.c_str(), W, theme_.pane_font_size, L"ja-jp");
    if (fmt_.titlebar_text) {
        fmt_.titlebar_text->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        fmt_.titlebar_text->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
        fmt_.titlebar_text->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    }
    fmt_.titlebar_icon = CreatePaneFormat(L"Segoe Fluent Icons", W, 14.0f, L"en-us");
    if (fmt_.titlebar_icon) {
        fmt_.titlebar_icon->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        fmt_.titlebar_icon->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
        fmt_.titlebar_icon->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    }

    fmt_.pane_icon = CreatePaneFormat(L"Segoe Fluent Icons", W, theme_.pane_font_size, L"en-us");
    if (fmt_.pane_icon) {
        fmt_.pane_icon->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        fmt_.pane_icon->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        fmt_.pane_icon->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
    }

    fmt_.pane_item = CreatePaneFormat(theme_.font_family.c_str(), W, theme_.pane_font_size, L"ja-jp");
    if (fmt_.pane_item) {
        fmt_.pane_item->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        fmt_.pane_item->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    }

    fmt_.pane_header = CreatePaneFormat(theme_.font_family.c_str(), DWRITE_FONT_WEIGHT_SEMI_BOLD, theme_.pane_font_size, L"ja-jp");
    if (fmt_.pane_header) {
        fmt_.pane_header->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        fmt_.pane_header->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    }

    // ナビゲーションオーバーレイボタンのテキストフォーマット（両軸中央揃え）
    fmt_.nav_button = CreatePaneFormat(theme_.font_family.c_str(), W, theme_.pane_font_size, L"ja-jp");
    if (fmt_.nav_button) {
        fmt_.nav_button->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        fmt_.nav_button->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
        fmt_.nav_button->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    }

    // ジェスチャーオーバーレイのテキストフォーマット（大きい太字、中央揃え）
    fmt_.gesture_overlay = CreatePaneFormat(theme_.font_family.c_str(), DWRITE_FONT_WEIGHT_BOLD, 32.0f * theme_.zoom, L"ja-JP");
    if (fmt_.gesture_overlay) {
        fmt_.gesture_overlay->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
        fmt_.gesture_overlay->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    }

    // トースト通知のテキストフォーマット（中央揃え）
    fmt_.toast_text = CreatePaneFormat(theme_.font_family.c_str(), DWRITE_FONT_WEIGHT_SEMI_BOLD, theme_.pane_font_size * 1.1f, L"ja-JP");
    if (fmt_.toast_text) {
        fmt_.toast_text->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
        fmt_.toast_text->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        fmt_.toast_text->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    }

    nav_back_layout_.Reset();
    nav_forward_layout_.Reset();
    if (fmt_.nav_button) {
        auto* dw = backend_.GetDWriteFactory();
        if (dw) {
            static const wchar_t kBack[] = L"\x25C0";
            static const wchar_t kForward[] = L"\x25B6";
            dw->CreateTextLayout(kBack, 1, fmt_.nav_button.Get(), NAV_BTN_SIZE, NAV_BTN_SIZE, &nav_back_layout_);
            dw->CreateTextLayout(kForward, 1, fmt_.nav_button.Get(), NAV_BTN_SIZE, NAV_BTN_SIZE, &nav_forward_layout_);
        }
    }

    // ペインキャッシュを無効化して新しいサイズで再描画させる
    file_pane_cache_.Reset();
    toc_pane_cache_.Reset();

    // コマンドジェネレータのフォーマットを更新
    cmd_generator_.SetFormats({ fmt_.list_number.Get(), fmt_.icon_font.Get(), fmt_.copy_btn_icon.Get() });
}

// ---- ノード描画 ----
// ノード描画ロジックはCommandGeneratorに抽出済み。
// D2Dブラシが必要なApplyNodeEffectsのみ描画前パスとしてここに残る。

void Renderer::ApplyVisibleEffects(std::pmr::vector<Node>& nodes, LayoutCache& cache,
    int first_visible, float viewport_bottom) {
    int node_count = static_cast<int>(nodes.size());
    for (int i = first_visible; i < node_count; i++) {
        if (cache[i].y_position > viewport_bottom) {
            break;
        }
        ApplyNodeEffects(nodes[i], cache[i]);
    }
}

ID2D1SolidColorBrush* Renderer::GetSyntaxBrush(SyntaxTokenType type) const {
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
    auto idx = static_cast<size_t>(type);
    if (idx >= std::size(SYNTAX_MAP)) {
        return nullptr;
    }
    return Brush(SYNTAX_MAP[idx]);
}

void Renderer::ApplyNodeEffects(const Node& node, NodeLayoutEntry& entry) {
    if (entry.effects_applied) {
        return;
    }
    entry.effects_applied = true;

    // テーブルセル: セルレイアウトにリンク色を適用
    if (node.type == NodeType::Table) {
        for (size_t r = 0; r < node.table_rows.size(); r++) {
            const auto& row = node.table_rows[r];
            for (size_t c = 0; c < row.cells.size(); c++) {
                IDWriteTextLayout* cell_layout = nullptr;
                if (r < entry.cell_layouts.size() && c < entry.cell_layouts[r].size()) {
                    cell_layout = entry.cell_layouts[r][c].Get();
                }
                if (!cell_layout) {
                    continue;
                }
                for (const auto& run : row.cells[c].runs) {
                    if (run.link_url.has_value()) {
                        DWRITE_TEXT_RANGE range{ run.start, run.length };
                        cell_layout->SetDrawingEffect(Brush(BrushId::Link), range);
                    }
                }
            }
        }
        return;
    }

    if (!entry.text_layout) {
        return;
    }

    // コードブロックにシンタックスハイライトを適用
    if (node.type == NodeType::CodeBlock) {
        for (const auto& token : node.syntax_tokens) {
            if (token.type == SyntaxTokenType::Plain) {
                continue;
            }
            auto* brush = GetSyntaxBrush(token.type);
            if (brush) {
                DWRITE_TEXT_RANGE range{ token.start, token.length };
                entry.text_layout->SetDrawingEffect(brush, range);
            }
        }
    }

    // Alertラベルの色を適用
    if (node.type == NodeType::BlockQuote && node.alert_type != AlertType::None
        && node.alert_label_length > 0) {
        static constexpr BrushId ALERT_BRUSH[] = {
            BrushId::AlertNote, BrushId::AlertTip, BrushId::AlertImportant,
            BrushId::AlertWarning, BrushId::AlertCaution,
        };
        static_assert(std::size(ALERT_BRUSH) == ALERT_TYPE_COUNT);
        auto idx = AlertColorIndex(node.alert_type);
        if (idx < ALERT_TYPE_COUNT) {
            DWRITE_TEXT_RANGE range{ 0, node.alert_label_length };
            entry.text_layout->SetDrawingEffect(Brush(ALERT_BRUSH[idx]), range);
        }
    }

    // リンクの下線/色を適用し、インラインコード背景の矩形をキャッシュ
    for (const auto& run : node.runs) {
        if (run.link_url.has_value()) {
            DWRITE_TEXT_RANGE range{ run.start, run.length };
            entry.text_layout->SetUnderline(TRUE, range);
            entry.text_layout->SetDrawingEffect(Brush(BrushId::Link), range);
        }
        if (run.code && node.type != NodeType::CodeBlock && run.length > 0) {
            UINT32 count = 0;
            entry.text_layout->HitTestTextRange(run.start, run.length, 0, 0, nullptr, 0, &count);
            if (count > 0) {
                hit_test_buffer_.resize(count);
                entry.text_layout->HitTestTextRange(run.start, run.length, 0, 0,
                    hit_test_buffer_.data(), count, &count);
                for (UINT32 i = 0; i < count; i++) {
                    entry.inline_code_bgs.push_back({
                        hit_test_buffer_[i].left,
                        hit_test_buffer_[i].top,
                        hit_test_buffer_[i].width,
                        hit_test_buffer_[i].height
                        });
                }
            }
        }
    }
}

// ---- メイン描画 ----

void Renderer::DrawSidePanes(const SidePaneState& sp) {
    if (sp.show_file_pane) {
        DrawFileExplorer(sp.file_entries, sp.file_pane_rect, sp.file_scroll, sp.hovered_file_index, sp.file_close_hovered, sp.file_refresh_hovered);
        DrawSplitter(sp.file_pane_rect.x + sp.file_pane_rect.width, sp.file_pane_rect.y, sp.file_pane_rect.y + sp.file_pane_rect.height);
    }
    if (sp.show_toc_pane) {
        DrawToc(sp.toc_entries, sp.toc_pane_rect, sp.toc_scroll, sp.hovered_toc_index, sp.toc_close_hovered);
        DrawSplitter(sp.toc_pane_rect.x + sp.toc_pane_rect.width, sp.toc_pane_rect.y, sp.toc_pane_rect.y + sp.toc_pane_rect.height);
    }
}

void Renderer::DrawLoading(float angle,
    const PaneRect& md_pane_rect,
    const SidePaneState& sp,
    const TitleBarRenderState& titlebar,
    const GestureRenderState& gesture,
    const ToastRenderState& toast) {
    if (!rt()) {
        return;
    }

    rt()->BeginDraw();
    rt()->Clear(theme_.bg_color);

    // カスタムタイトルバーを描画
    DrawTitleBar(titlebar);

    DrawSidePanes(sp);

    // MDペイン中央にスピナーを描画
    float cx = md_pane_rect.x + md_pane_rect.width / 2.0f;
    float cy = md_pane_rect.y + md_pane_rect.height / 2.0f;
    for (int i = 0; i < spinner::DOT_COUNT; i++) {
        float a = angle - i * (TWO_PI / spinner::DOT_COUNT);
        float dx = cx + spinner::RADIUS * std::cos(a);
        float dy = cy + spinner::RADIUS * std::sin(a);
        float alpha = 1.0f - i * (spinner::DOT_FADE_FACTOR / spinner::DOT_COUNT);

        D2D1_ELLIPSE ellipse = D2D1::Ellipse(D2D1::Point2F(dx, dy), spinner::DOT_RADIUS, spinner::DOT_RADIUS);
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

void Renderer::Render(const RenderParams& p) {
    if (!rt()) {
        return;
    }

    rt()->BeginDraw();
    rt()->Clear(theme_.bg_color);

    // カスタムタイトルバーを描画
    DrawTitleBar(p.titlebar);

    DrawSidePanes(p.side_panes);

    // 最初の可視ノードを検索（一度だけ実行し、エフェクトとコマンド生成で共有）。
    // ヒットテストとの座標一致のためスナップ前の scroll_y を使う。
    float viewport_top = p.scroll_y;
    float viewport_bottom = p.scroll_y + p.md_pane_rect.height;
    int first_visible = FindFirstVisibleNodeIndex(p.cache, p.nodes.size(), viewport_top);

    // 描画前パス: 可視ノードに描画エフェクト（シンタックスハイライト、リンク色）を適用。
    ApplyVisibleEffects(p.nodes, p.cache, first_visible, viewport_bottom);

    // Markdownコンテンツペインの描画コマンドを生成・実行。
    float dpi_scale = backend_.GetDpi() / DEFAULT_DPI;
    const auto& cmds = cmd_generator_.GenerateMdPane(p.nodes, p.cache, p.md_pane_rect, p.scroll_y, p.selection, first_visible, p.hovered_copy_node, dpi_scale);
    cmd_executor_.Execute(cmds, rt());

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

    // Markdownペインのカスタムスクロールバー
    DrawMdScrollbar(p.md_pane_rect, p.scroll_y, p.total_content_height);

    if (!CheckEndDraw()) {
        return;
    }
}

bool Renderer::CheckEndDraw() {
    HRESULT hr = rt()->EndDraw();
    if (hr == D2DERR_RECREATE_TARGET) {
        RecreateRenderTarget();
        // 現在のフレームは破棄された — 新しいターゲットで再描画を要求
        InvalidateRect(backend_.GetHwnd(), nullptr, FALSE);
        return false;
    }
    return SUCCEEDED(hr);
}

bool Renderer::RecreateRenderTarget() {
    if (!backend_.RecreateRenderTarget()) {
        return false;
    }

    RecreateBrushes();
    file_pane_cache_.Reset();
    toc_pane_cache_.Reset();
    cmd_executor_ = CommandExecutor{}; // バインドされたレンダーターゲットをリセット

    // 依存リソース（例: MermaidRendererのビットマップ）が更新されるようオーナーに通知
    if (on_device_lost_) {
        on_device_lost_(backend_.GetRenderTarget());
    }

    return true;
}

void Renderer::DrawNavOverlay(const PaneRect& md_pane_rect,
    bool can_back, bool can_forward,
    int hovered) {
    if (!rt()) {
        return;
    }

    bool is_dark = theme_.IsDark();

    // 位置: MDペインの右下にマージン付き
    float base_x = md_pane_rect.x + md_pane_rect.width - NAV_BTN_MARGIN - NAV_BTN_SIZE * 2 - NAV_BTN_GAP - NAV_BTN_SCROLLBAR_OFFSET;
    float base_y = md_pane_rect.y + md_pane_rect.height - NAV_BTN_MARGIN - NAV_BTN_SIZE;

    auto drawButton = [&](float x, bool enabled, bool is_hovered, IDWriteTextLayout* arrow_layout) {
        if (!Brush(BrushId::Overlay)) {
            return;
        }
        D2D1_RECT_F rect = D2D1::RectF(x, base_y, x + NAV_BTN_SIZE, base_y + NAV_BTN_SIZE);

        // 背景
        float bg_alpha;
        if (!enabled) {
            bg_alpha = is_dark ? 0.08f : 0.05f;
        }
        else if (is_hovered) {
            bg_alpha = is_dark ? 0.35f : 0.25f;
        }
        else {
            bg_alpha = is_dark ? 0.15f : 0.10f;
        }

        D2D1_COLOR_F bg_color = is_dark
            ? D2D1::ColorF(1.0f, 1.0f, 1.0f, bg_alpha)
            : D2D1::ColorF(0.0f, 0.0f, 0.0f, bg_alpha);

        Brush(BrushId::Overlay)->SetColor(bg_color);
        D2D1_ROUNDED_RECT rrect = D2D1::RoundedRect(rect, NAV_BTN_CORNER, NAV_BTN_CORNER);
        rt()->FillRoundedRectangle(rrect, Brush(BrushId::Overlay));

        // 矢印テキスト（キャッシュ済みレイアウトを使用）
        float text_alpha;
        if (!enabled) {
            text_alpha = is_dark ? 0.2f : 0.15f;
        }
        else if (is_hovered) {
            text_alpha = 1.0f;
        }
        else {
            text_alpha = is_dark ? 0.6f : 0.5f;
        }

        D2D1_COLOR_F text_color = is_dark
            ? D2D1::ColorF(1.0f, 1.0f, 1.0f, text_alpha)
            : D2D1::ColorF(0.0f, 0.0f, 0.0f, text_alpha);

        if (arrow_layout) {
            Brush(BrushId::Overlay)->SetColor(text_color);
            rt()->DrawTextLayout(D2D1::Point2F(x, base_y), arrow_layout, Brush(BrushId::Overlay));
        }
    };

    // 戻るボタン (◀)
    drawButton(base_x, can_back, hovered == 1, nav_back_layout_.Get());
    // 進むボタン (▶)
    drawButton(base_x + NAV_BTN_SIZE + NAV_BTN_GAP, can_forward, hovered == 2, nav_forward_layout_.Get());
}

void Renderer::DrawGestureTrail(const std::pmr::deque<GesturePoint>& points) {
    if (!rt() || points.size() < 2) {
        return;
    }
    if (!Brush(BrushId::Overlay) || !d2d()) {
        return;
    }

    // パスジオメトリで一筆描きすることで、結合部のアルファ蓄積（節）を防ぐ
    ComPtr<ID2D1PathGeometry> path;
    if (FAILED(d2d()->CreatePathGeometry(&path))) {
        return;
    }

    ComPtr<ID2D1GeometrySink> sink;
    if (FAILED(path->Open(&sink))) {
        return;
    }

    sink->BeginFigure(D2D1::Point2F(points[0].x, points[0].y), D2D1_FIGURE_BEGIN_HOLLOW);
    for (size_t i = 1; i < points.size(); i++) {
        sink->AddLine(D2D1::Point2F(points[i].x, points[i].y));
    }
    sink->EndFigure(D2D1_FIGURE_END_OPEN);
    if (FAILED(sink->Close())) {
        return;
    }

    Brush(BrushId::Overlay)->SetColor(D2D1::ColorF(0.9f, 0.2f, 0.2f, 0.5f));
    rt()->DrawGeometry(path.Get(), Brush(BrushId::Overlay), 4.0f, gesture_stroke_style_.Get());
}

void Renderer::DrawGestureOverlay(int direction, float alpha, const PaneRect& md_pane_rect) {
    if (!rt() || direction == 0 || !Brush(BrushId::Overlay)) {
        return;
    }

    bool is_dark = theme_.IsDark();

    // MDペイン中央の矩形
    float rect_w = 280.0f;
    float rect_h = 80.0f;
    float cx = md_pane_rect.x + md_pane_rect.width / 2.0f;
    float cy = md_pane_rect.y + md_pane_rect.height / 2.0f;
    D2D1_RECT_F rect = D2D1::RectF(cx - rect_w / 2, cy - rect_h / 2,
        cx + rect_w / 2, cy + rect_h / 2);

    // 背景（両テーマ共通の半透明ダークオーバーレイ）
    D2D1_COLOR_F bg_color = is_dark
        ? D2D1::ColorF(0.2f, 0.2f, 0.2f, alpha * 0.8f)
        : D2D1::ColorF(0.0f, 0.0f, 0.0f, alpha * 0.6f);

    Brush(BrushId::Overlay)->SetColor(bg_color);
    D2D1_ROUNDED_RECT rrect = D2D1::RoundedRect(rect, 12.0f, 12.0f);
    rt()->FillRoundedRectangle(rrect, Brush(BrushId::Overlay));

    // テキスト（両テーマ共通でダークオーバーレイ上に白色）
    const wchar_t* text = (direction < 0) ? L"\x2190 \x623B\x308B" : L"\x2192 \x9032\x3080";
    UINT32 text_len = static_cast<UINT32>(wcslen(text));

    if (fmt_.gesture_overlay) {
        Brush(BrushId::Overlay)->SetColor(D2D1::ColorF(1.0f, 1.0f, 1.0f, alpha));
        rt()->DrawText(
            text, text_len, fmt_.gesture_overlay.Get(), rect, Brush(BrushId::Overlay),
            D2D1_DRAW_TEXT_OPTIONS_NONE, DWRITE_MEASURING_MODE_NATURAL);
    }
}

void Renderer::DrawToastOverlay(const ToastRenderState& toast, const PaneRect& md_pane_rect) {
    if (!rt() || toast.message.empty() || !Brush(BrushId::Overlay)) {
        return;
    }

    float alpha = std::min(toast.alpha, 1.0f);
    bool is_dark = theme_.IsDark();

    // MDペイン下部中央に配置
    float rect_w = 320.0f;
    float rect_h = 48.0f;
    float cx = md_pane_rect.x + md_pane_rect.width / 2.0f;
    float bottom_y = md_pane_rect.y + md_pane_rect.height - NAV_BTN_MARGIN - NAV_BTN_SIZE - 16.0f;
    D2D1_RECT_F rect = D2D1::RectF(cx - rect_w / 2, bottom_y - rect_h,
        cx + rect_w / 2, bottom_y);

    // 半透明ダーク背景
    D2D1_COLOR_F bg_color = is_dark
        ? D2D1::ColorF(0.2f, 0.2f, 0.2f, alpha * 0.85f)
        : D2D1::ColorF(0.0f, 0.0f, 0.0f, alpha * 0.7f);
    Brush(BrushId::Overlay)->SetColor(bg_color);
    D2D1_ROUNDED_RECT rrect = D2D1::RoundedRect(rect, 8.0f, 8.0f);
    rt()->FillRoundedRectangle(rrect, Brush(BrushId::Overlay));

    // 白テキスト
    if (fmt_.toast_text) {
        Brush(BrushId::Overlay)->SetColor(D2D1::ColorF(1.0f, 1.0f, 1.0f, alpha));
        rt()->DrawText(
            toast.message.data(), static_cast<UINT32>(toast.message.size()),
            fmt_.toast_text.Get(), rect, Brush(BrushId::Overlay),
            D2D1_DRAW_TEXT_OPTIONS_NONE, DWRITE_MEASURING_MODE_NATURAL);
    }
}
