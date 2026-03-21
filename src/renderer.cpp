#include "renderer.h"
#include <algorithm>
#include <cmath>

#pragma comment(lib, "d2d1.lib")
#pragma comment(lib, "dwrite.lib")

bool Renderer::Init(HWND hwnd) {
    theme_ = GetLightTheme();

    if (!backend_.Init(hwnd)) return false;

    RecreateBrushes();
    RecreatePaneFormats();

    // Round caps and joins for smooth gesture trail
    D2D1_STROKE_STYLE_PROPERTIES ssp = D2D1::StrokeStyleProperties(
        D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND,
        D2D1_CAP_STYLE_ROUND, D2D1_LINE_JOIN_ROUND);
    backend_.GetD2DFactory()->CreateStrokeStyle(ssp, nullptr, 0, &gesture_stroke_style_);

    measurer_.SetFactory(backend_.GetDWriteFactory());
    if (!layout_.Init(&measurer_, theme_)) return false;

    cmd_generator_.SetTheme(&theme_);
    cmd_generator_.SetFormats({fmt_list_number_.Get(), icon_font_format_.Get()});

    return true;
}

void Renderer::RecreateBrushes() {
    auto* render_target_ = backend_.GetRenderTarget();
    if (!render_target_) return;

    for (auto& b : brushes_) b.Reset();

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
        {BrushId::Selection,        D2D1::ColorF(0.26f, 0.56f, 0.84f, 0.3f)},
        {BrushId::TableStripe,      is_dark ? D2D1::ColorF(1.0f, 1.0f, 1.0f, stripe_alpha)
                                            : D2D1::ColorF(0.0f, 0.0f, 0.0f, stripe_alpha)},
        {BrushId::SyntaxKeyword,    theme_.syntax_keyword},
        {BrushId::SyntaxType,       theme_.syntax_type},
        {BrushId::SyntaxString,     theme_.syntax_string},
        {BrushId::SyntaxNumber,     theme_.syntax_number},
        {BrushId::SyntaxComment,    theme_.syntax_comment},
        {BrushId::SyntaxPreprocessor, theme_.syntax_preprocessor},
        {BrushId::SyntaxFunction,   theme_.syntax_function},
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
    if (!backend_.GetRenderTarget()) return;
    RecreateBrushes();
    RecreatePaneFormats();
    cmd_generator_.SetTheme(&theme_);
}

void Renderer::Resize(UINT width, UINT height) {
    backend_.Resize(width, height);
}

void Renderer::SetDpi(float dpi) {
    backend_.SetDpi(dpi);
    // Force pane cache recreation at new DPI
    file_pane_cache_.Reset();
    toc_pane_cache_.Reset();
}

void Renderer::ApplyZoom(float new_zoom) {
    theme_.ApplyZoom(new_zoom);
    layout_.UpdateTheme(theme_);
    layout_.RecreateFormats();
    RecreatePaneFormats();
}

void Renderer::ApplyZoomFromBase(const Theme& base_theme, float new_zoom) {
    theme_ = base_theme;
    if (new_zoom != 1.0f) {
        theme_.ApplyZoom(new_zoom);
    }
    layout_.UpdateTheme(theme_);
    layout_.RecreateFormats();
    RecreatePaneFormats();
    cmd_generator_.SetTheme(&theme_);
}

void Renderer::RecreatePaneFormats() {
    // Recreate all pane / UI text formats at updated theme sizes
    icon_font_format_.Reset();
    fmt_list_number_.Reset();
    fmt_pane_icon_.Reset();
    fmt_pane_item_.Reset();
    fmt_pane_header_.Reset();
    fmt_nav_button_.Reset();
    fmt_gesture_overlay_.Reset();

    backend_.GetDWriteFactory()->CreateTextFormat(
        L"Segoe Fluent Icons", nullptr,
        DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL,
        DWRITE_FONT_STRETCH_NORMAL, theme_.font_size_body,
        L"en-us", &icon_font_format_);

    // List number format (right-aligned for ordered list bullets)
    backend_.GetDWriteFactory()->CreateTextFormat(
        theme_.font_family, nullptr,
        DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL,
        DWRITE_FONT_STRETCH_NORMAL, theme_.font_size_body,
        L"ja-jp", &fmt_list_number_);
    if (fmt_list_number_) {
        fmt_list_number_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_TRAILING);
    }

    backend_.GetDWriteFactory()->CreateTextFormat(
        L"Segoe Fluent Icons", nullptr,
        DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL,
        DWRITE_FONT_STRETCH_NORMAL, theme_.pane_font_size,
        L"en-us", &fmt_pane_icon_);
    if (fmt_pane_icon_) {
        fmt_pane_icon_->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        fmt_pane_icon_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        fmt_pane_icon_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
    }

    backend_.GetDWriteFactory()->CreateTextFormat(
        theme_.font_family, nullptr,
        DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL,
        DWRITE_FONT_STRETCH_NORMAL, theme_.pane_font_size,
        L"ja-jp", &fmt_pane_item_);
    if (fmt_pane_item_) {
        fmt_pane_item_->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        fmt_pane_item_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    }

    backend_.GetDWriteFactory()->CreateTextFormat(
        theme_.font_family, nullptr,
        DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_FONT_STYLE_NORMAL,
        DWRITE_FONT_STRETCH_NORMAL, theme_.pane_font_size,
        L"ja-jp", &fmt_pane_header_);
    if (fmt_pane_header_) {
        fmt_pane_header_->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        fmt_pane_header_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    }

    // Navigation overlay button text format (centered both axes)
    backend_.GetDWriteFactory()->CreateTextFormat(
        theme_.font_family, nullptr,
        DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL,
        DWRITE_FONT_STRETCH_NORMAL, theme_.pane_font_size,
        L"ja-jp", &fmt_nav_button_);
    if (fmt_nav_button_) {
        fmt_nav_button_->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        fmt_nav_button_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
        fmt_nav_button_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    }

    // Gesture overlay text format (large bold, centered)
    backend_.GetDWriteFactory()->CreateTextFormat(
        theme_.font_family, nullptr,
        DWRITE_FONT_WEIGHT_BOLD, DWRITE_FONT_STYLE_NORMAL,
        DWRITE_FONT_STRETCH_NORMAL, 32.0f * theme_.zoom,
        L"ja-JP", &fmt_gesture_overlay_);
    if (fmt_gesture_overlay_) {
        fmt_gesture_overlay_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
        fmt_gesture_overlay_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    }

    // Invalidate pane caches so they redraw with new sizes
    file_pane_cache_.Reset();
    toc_pane_cache_.Reset();

    // Update command generator formats
    cmd_generator_.SetFormats({fmt_list_number_.Get(), icon_font_format_.Get()});
}

// ---- Node drawing ----
// Node drawing logic has been extracted to CommandGenerator.
// Only ApplyNodeEffects remains here as a pre-pass (requires D2D brushes).

void Renderer::ApplyVisibleEffects(std::pmr::vector<Node>& nodes, LayoutCache& cache,
                                    int first_visible, float viewport_bottom) {
    int node_count = static_cast<int>(nodes.size());
    for (int i = first_visible; i < node_count; i++) {
        if (cache[i].y_position > viewport_bottom) break;
        ApplyNodeEffects(nodes[i], cache[i]);
    }
}

ID2D1SolidColorBrush* Renderer::GetSyntaxBrush(SyntaxTokenType type) const {
    static constexpr BrushId SYNTAX_MAP[] = {
        BrushId::Text,                // Plain (unused, returns text brush as fallback)
        BrushId::SyntaxKeyword,       // Keyword
        BrushId::SyntaxType,          // Type
        BrushId::SyntaxString,        // String
        BrushId::SyntaxNumber,        // Number
        BrushId::SyntaxComment,       // Comment
        BrushId::SyntaxPreprocessor,  // Preprocessor
        BrushId::SyntaxFunction,      // Function
    };
    auto idx = static_cast<size_t>(type);
    if (idx >= std::size(SYNTAX_MAP)) return nullptr;
    return Brush(SYNTAX_MAP[idx]);
}

void Renderer::ApplyNodeEffects(const Node& node, NodeLayoutEntry& entry) {
    if (entry.effects_applied) return;
    entry.effects_applied = true;

    // Table cells: apply link colors on cell layouts
    if (node.type == NodeType::Table) {
        for (size_t r = 0; r < node.table_rows.size(); r++) {
            const auto& row = node.table_rows[r];
            for (size_t c = 0; c < row.cells.size(); c++) {
                IDWriteTextLayout* cell_layout = nullptr;
                if (r < entry.cell_layouts.size() && c < entry.cell_layouts[r].size())
                    cell_layout = entry.cell_layouts[r][c].Get();
                if (!cell_layout) continue;
                for (const auto& run : row.cells[c].runs) {
                    if (run.link_url.has_value()) {
                        DWRITE_TEXT_RANGE range{run.start, run.length};
                        cell_layout->SetDrawingEffect(Brush(BrushId::Link), range);
                    }
                }
            }
        }
        return;
    }

    if (!entry.text_layout) return;

    // Apply syntax highlighting for code blocks
    if (node.type == NodeType::CodeBlock) {
        for (const auto& token : node.syntax_tokens) {
            if (token.type == SyntaxTokenType::Plain) continue;
            auto* brush = GetSyntaxBrush(token.type);
            if (brush) {
                DWRITE_TEXT_RANGE range{token.start, token.length};
                entry.text_layout->SetDrawingEffect(brush, range);
            }
        }
    }

    // Apply link underlines/colors and cache inline code background rects
    for (const auto& run : node.runs) {
        if (run.link_url.has_value()) {
            DWRITE_TEXT_RANGE range{run.start, run.length};
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

// ---- Main rendering ----

void Renderer::DrawLoading(float angle,
                            const PaneRect& md_pane_rect,
                            const SidePaneState& sp,
                            const GestureRenderState& gesture) {
    if (!rt()) return;

    rt()->BeginDraw();
    rt()->Clear(theme_.bg_color);

    auto size = rt()->GetSize();

    // Draw side panes normally
    if (sp.show_file_pane) {
        DrawFileExplorer(sp.file_entries, sp.file_pane_rect, sp.file_scroll, sp.hovered_file_index);
        DrawSplitter(sp.file_pane_rect.x + sp.file_pane_rect.width, size.height);
    }
    if (sp.show_toc_pane) {
        DrawToc(sp.toc_entries, sp.toc_pane_rect, sp.toc_scroll, sp.hovered_toc_index);
        DrawSplitter(sp.toc_pane_rect.x + sp.toc_pane_rect.width, size.height);
    }

    // Draw spinner in center of MD pane
    float cx = md_pane_rect.x + md_pane_rect.width / 2.0f;
    float cy = md_pane_rect.y + md_pane_rect.height / 2.0f;
    float radius = 20.0f;
    float dot_radius = 3.0f;
    constexpr int dot_count = 8;
    constexpr float pi2 = 6.28318530f;

    for (int i = 0; i < dot_count; i++) {
        float a = angle - i * (pi2 / dot_count);
        float dx = cx + radius * std::cos(a);
        float dy = cy + radius * std::sin(a);
        float alpha = 1.0f - i * (0.85f / dot_count);

        D2D1_ELLIPSE ellipse = D2D1::Ellipse(D2D1::Point2F(dx, dy), dot_radius, dot_radius);
        Brush(BrushId::Text)->SetOpacity(alpha);
        rt()->FillEllipse(ellipse, Brush(BrushId::Text));
    }
    Brush(BrushId::Text)->SetOpacity(1.0f);

    // Gesture overlay (visible during fade-out even while loading)
    if (gesture.overlay_visible && gesture.overlay_alpha > 0.0f) {
        DrawGestureOverlay(gesture.direction, gesture.overlay_alpha, md_pane_rect);
    }

    if (!CheckEndDraw()) return;
}

void Renderer::Render(std::pmr::vector<Node>& nodes, LayoutCache& cache, float scroll_y,
                      const TextSelection& selection,
                      const PaneRect& md_pane_rect,
                      const SidePaneState& sp,
                      bool can_go_back, bool can_go_forward,
                      int nav_hovered,
                      const GestureRenderState& gesture) {
    if (!rt()) return;

    rt()->BeginDraw();
    rt()->Clear(theme_.bg_color);

    auto size = rt()->GetSize();

    // Draw file explorer pane (bitmap blit when cache is clean)
    if (sp.show_file_pane) {
        DrawFileExplorer(sp.file_entries, sp.file_pane_rect, sp.file_scroll, sp.hovered_file_index);
        DrawSplitter(sp.file_pane_rect.x + sp.file_pane_rect.width, size.height);
    }

    // Draw TOC pane (bitmap blit when cache is clean)
    if (sp.show_toc_pane) {
        DrawToc(sp.toc_entries, sp.toc_pane_rect, sp.toc_scroll, sp.hovered_toc_index);
        DrawSplitter(sp.toc_pane_rect.x + sp.toc_pane_rect.width, size.height);
    }

    // Find first visible node (done once, shared by effects + command gen).
    float viewport_top = scroll_y;
    float viewport_bottom = scroll_y + md_pane_rect.height;
    int first_visible = FindFirstVisibleNodeIndex(cache, nodes.size(), viewport_top);

    // Pre-pass: apply drawing effects (syntax highlighting, link colors) on visible nodes.
    ApplyVisibleEffects(nodes, cache, first_visible, viewport_bottom);

    // Generate and execute draw commands for the Markdown content pane.
    const auto& cmds = cmd_generator_.GenerateMdPane(nodes, cache, md_pane_rect, scroll_y, selection, first_visible);
    cmd_executor_.Execute(cmds, rt());

    // Draw navigation overlay buttons (back/forward)
    if (can_go_back || can_go_forward) {
        DrawNavOverlay(md_pane_rect, can_go_back, can_go_forward, nav_hovered);
    }

    // Gesture trail
    if (gesture.trail_active && gesture.trail_points && gesture.trail_points->size() >= 2) {
        DrawGestureTrail(*gesture.trail_points);
    }

    // Gesture overlay (fade-out after action)
    if (gesture.overlay_visible && gesture.overlay_alpha > 0.0f) {
        DrawGestureOverlay(gesture.direction, gesture.overlay_alpha, md_pane_rect);
    }

    if (!CheckEndDraw()) return;
}

bool Renderer::CheckEndDraw() {
    HRESULT hr = rt()->EndDraw();
    if (hr == D2DERR_RECREATE_TARGET) {
        RecreateRenderTarget();
        // Current frame was discarded — request a repaint on the new target
        InvalidateRect(backend_.GetHwnd(), nullptr, FALSE);
        return false;
    }
    return SUCCEEDED(hr);
}

bool Renderer::RecreateRenderTarget() {
    if (!backend_.RecreateRenderTarget()) return false;

    RecreateBrushes();
    file_pane_cache_.Reset();
    toc_pane_cache_.Reset();
    cmd_executor_ = CommandExecutor{}; // Reset bound render target

    // Notify owner so dependent resources (e.g. MermaidRenderer bitmaps) are updated
    if (on_device_lost_) {
        on_device_lost_(backend_.GetRenderTarget());
    }

    return true;
}

// Navigation overlay constants
#include "nav_button_constants.h"

void Renderer::DrawNavOverlay(const PaneRect& md_pane_rect,
                              bool can_back, bool can_forward,
                              int hovered) {
    if (!rt()) return;

    bool is_dark = theme_.IsDark();

    // Position: bottom-right of MD pane with margin
    float base_x = md_pane_rect.x + md_pane_rect.width - NAV_BTN_MARGIN - NAV_BTN_SIZE * 2 - NAV_BTN_GAP - NAV_BTN_SCROLLBAR_OFFSET;
    float base_y = md_pane_rect.y + md_pane_rect.height - NAV_BTN_MARGIN - NAV_BTN_SIZE;

    auto drawButton = [&](float x, bool enabled, bool is_hovered, const wchar_t* arrow) {
        if (!Brush(BrushId::Overlay)) return;
        D2D1_RECT_F rect = D2D1::RectF(x, base_y, x + NAV_BTN_SIZE, base_y + NAV_BTN_SIZE);

        // Background
        float bg_alpha;
        if (!enabled)        bg_alpha = is_dark ? 0.08f : 0.05f;
        else if (is_hovered) bg_alpha = is_dark ? 0.35f : 0.25f;
        else                 bg_alpha = is_dark ? 0.15f : 0.10f;

        D2D1_COLOR_F bg_color = is_dark
            ? D2D1::ColorF(1.0f, 1.0f, 1.0f, bg_alpha)
            : D2D1::ColorF(0.0f, 0.0f, 0.0f, bg_alpha);

        Brush(BrushId::Overlay)->SetColor(bg_color);
        D2D1_ROUNDED_RECT rrect = D2D1::RoundedRect(rect, NAV_BTN_CORNER, NAV_BTN_CORNER);
        rt()->FillRoundedRectangle(rrect, Brush(BrushId::Overlay));

        // Arrow text
        float text_alpha;
        if (!enabled)        text_alpha = is_dark ? 0.2f : 0.15f;
        else if (is_hovered) text_alpha = 1.0f;
        else                 text_alpha = is_dark ? 0.6f : 0.5f;

        D2D1_COLOR_F text_color = is_dark
            ? D2D1::ColorF(1.0f, 1.0f, 1.0f, text_alpha)
            : D2D1::ColorF(0.0f, 0.0f, 0.0f, text_alpha);

        if (fmt_nav_button_) {
            Brush(BrushId::Overlay)->SetColor(text_color);
            rt()->DrawText(
                arrow, 1, fmt_nav_button_.Get(), rect, Brush(BrushId::Overlay),
                D2D1_DRAW_TEXT_OPTIONS_NONE, DWRITE_MEASURING_MODE_NATURAL);
        }
    };

    // Back button (◀)
    drawButton(base_x, can_back, hovered == 1, L"\x25C0");
    // Forward button (▶)
    drawButton(base_x + NAV_BTN_SIZE + NAV_BTN_GAP, can_forward, hovered == 2, L"\x25B6");
}

void Renderer::DrawGestureTrail(const std::pmr::deque<GesturePoint>& points) {
    if (!rt() || !d2d() || points.size() < 2) return;

    ComPtr<ID2D1PathGeometry> geometry;
    if (FAILED(d2d()->CreatePathGeometry(&geometry))) return;

    ComPtr<ID2D1GeometrySink> sink;
    if (FAILED(geometry->Open(&sink))) return;

    sink->BeginFigure(D2D1::Point2F(points[0].x, points[0].y), D2D1_FIGURE_BEGIN_HOLLOW);
    for (size_t i = 1; i < points.size(); i++) {
        sink->AddLine(D2D1::Point2F(points[i].x, points[i].y));
    }
    sink->EndFigure(D2D1_FIGURE_END_OPEN);
    sink->Close();

    if (!Brush(BrushId::Overlay)) return;
    Brush(BrushId::Overlay)->SetColor(D2D1::ColorF(0.9f, 0.2f, 0.2f, 0.5f));

    rt()->DrawGeometry(geometry.Get(), Brush(BrushId::Overlay), 4.0f, gesture_stroke_style_.Get());
}

void Renderer::DrawGestureOverlay(int direction, float alpha, const PaneRect& md_pane_rect) {
    if (!rt() || direction == 0 || !Brush(BrushId::Overlay)) return;

    bool is_dark = theme_.IsDark();

    // Center rectangle in MD pane
    float rect_w = 280.0f;
    float rect_h = 80.0f;
    float cx = md_pane_rect.x + md_pane_rect.width / 2.0f;
    float cy = md_pane_rect.y + md_pane_rect.height / 2.0f;
    D2D1_RECT_F rect = D2D1::RectF(cx - rect_w / 2, cy - rect_h / 2,
                                     cx + rect_w / 2, cy + rect_h / 2);

    // Background (semi-transparent dark overlay for both themes)
    D2D1_COLOR_F bg_color = is_dark
        ? D2D1::ColorF(0.2f, 0.2f, 0.2f, alpha * 0.8f)
        : D2D1::ColorF(0.0f, 0.0f, 0.0f, alpha * 0.6f);

    Brush(BrushId::Overlay)->SetColor(bg_color);
    D2D1_ROUNDED_RECT rrect = D2D1::RoundedRect(rect, 12.0f, 12.0f);
    rt()->FillRoundedRectangle(rrect, Brush(BrushId::Overlay));

    // Text (white on dark overlay for both themes)
    const wchar_t* text = (direction < 0) ? L"\x2190 \x623B\x308B" : L"\x2192 \x9032\x3080";
    UINT32 text_len = static_cast<UINT32>(wcslen(text));

    if (fmt_gesture_overlay_) {
        Brush(BrushId::Overlay)->SetColor(D2D1::ColorF(1.0f, 1.0f, 1.0f, alpha));
        rt()->DrawText(
            text, text_len, fmt_gesture_overlay_.Get(), rect, Brush(BrushId::Overlay),
            D2D1_DRAW_TEXT_OPTIONS_NONE, DWRITE_MEASURING_MODE_NATURAL);
    }
}
