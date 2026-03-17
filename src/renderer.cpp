#include "renderer.h"
#include <algorithm>
#include <cmath>

#pragma comment(lib, "d2d1.lib")
#pragma comment(lib, "dwrite.lib")

bool Renderer::Init(HWND hwnd) {
    hwnd_ = hwnd;
    theme_ = GetLightTheme();

    // Query the actual DPI for this window's monitor
    dpi_ = static_cast<float>(GetDpiForWindow(hwnd));
    if (dpi_ == 0.0f) dpi_ = 96.0f;

    // Create D2D factory
    HRESULT hr = D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, d2d_factory_.GetAddressOf());
    if (FAILED(hr)) return false;

    // Create DirectWrite factory
    hr = DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED,
        __uuidof(IDWriteFactory),
        reinterpret_cast<IUnknown**>(dwrite_factory_.GetAddressOf()));
    if (FAILED(hr)) return false;

    // Create render target with correct initial DPI
    RECT rc;
    GetClientRect(hwnd, &rc);

    D2D1_RENDER_TARGET_PROPERTIES rtProps = D2D1::RenderTargetProperties();
    rtProps.dpiX = dpi_;
    rtProps.dpiY = dpi_;

    D2D1_HWND_RENDER_TARGET_PROPERTIES hwndProps = D2D1::HwndRenderTargetProperties(
        hwnd, D2D1::SizeU(rc.right - rc.left, rc.bottom - rc.top));
    hwndProps.presentOptions = D2D1_PRESENT_OPTIONS_NONE;

    hr = d2d_factory_->CreateHwndRenderTarget(rtProps, hwndProps, &render_target_);
    if (FAILED(hr)) return false;

    // Create all brushes from theme
    RecreateBrushes();

    // Create pane text formats
    RecreatePaneFormats();

    // Initialize layout engine via DWriteTextMeasurer
    measurer_.SetFactory(dwrite_factory_.Get());
    if (!layout_.Init(&measurer_, theme_)) return false;

    // Initialize command generator
    cmd_generator_.SetTheme(&theme_);
    cmd_generator_.SetFormats({fmt_list_number_.Get(), icon_font_format_.Get()});

    return true;
}

void Renderer::RecreateBrushes() {
    if (!render_target_) return;

    // Reset all brushes
    text_brush_.Reset();
    heading_brush_.Reset();
    code_bg_brush_.Reset();
    code_text_brush_.Reset();
    link_brush_.Reset();
    hr_brush_.Reset();
    blockquote_bar_brush_.Reset();
    blockquote_text_brush_.Reset();
    selection_brush_.Reset();
    table_stripe_brush_.Reset();

    syntax_keyword_brush_.Reset();
    syntax_type_brush_.Reset();
    syntax_string_brush_.Reset();
    syntax_number_brush_.Reset();
    syntax_comment_brush_.Reset();
    syntax_preprocessor_brush_.Reset();
    syntax_function_brush_.Reset();

    pane_bg_brush_.Reset();
    splitter_brush_.Reset();
    pane_item_hover_brush_.Reset();
    pane_item_active_brush_.Reset();
    scrollbar_thumb_brush_.Reset();

    // Create theme brushes
    render_target_->CreateSolidColorBrush(theme_.text_color, &text_brush_);
    render_target_->CreateSolidColorBrush(theme_.heading_color, &heading_brush_);
    render_target_->CreateSolidColorBrush(theme_.code_bg_color, &code_bg_brush_);
    render_target_->CreateSolidColorBrush(theme_.code_text_color, &code_text_brush_);
    render_target_->CreateSolidColorBrush(theme_.link_color, &link_brush_);
    render_target_->CreateSolidColorBrush(theme_.hr_color, &hr_brush_);
    render_target_->CreateSolidColorBrush(theme_.blockquote_bar_color, &blockquote_bar_brush_);
    render_target_->CreateSolidColorBrush(theme_.blockquote_text_color, &blockquote_text_brush_);

    // Theme-dependent colors
    bool is_dark = (theme_.bg_color.r + theme_.bg_color.g + theme_.bg_color.b) < 1.5f;

    render_target_->CreateSolidColorBrush(D2D1::ColorF(0.26f, 0.56f, 0.84f, 0.3f), &selection_brush_);

    float stripe_alpha = is_dark ? 0.05f : 0.02f;
    D2D1_COLOR_F stripe_color = is_dark
        ? D2D1::ColorF(1.0f, 1.0f, 1.0f, stripe_alpha)
        : D2D1::ColorF(0.0f, 0.0f, 0.0f, stripe_alpha);
    render_target_->CreateSolidColorBrush(stripe_color, &table_stripe_brush_);

    // Syntax highlighting brushes
    render_target_->CreateSolidColorBrush(theme_.syntax_keyword, &syntax_keyword_brush_);
    render_target_->CreateSolidColorBrush(theme_.syntax_type, &syntax_type_brush_);
    render_target_->CreateSolidColorBrush(theme_.syntax_string, &syntax_string_brush_);
    render_target_->CreateSolidColorBrush(theme_.syntax_number, &syntax_number_brush_);
    render_target_->CreateSolidColorBrush(theme_.syntax_comment, &syntax_comment_brush_);
    render_target_->CreateSolidColorBrush(theme_.syntax_preprocessor, &syntax_preprocessor_brush_);
    render_target_->CreateSolidColorBrush(theme_.syntax_function, &syntax_function_brush_);

    // Pane brushes
    render_target_->CreateSolidColorBrush(theme_.pane_bg_color, &pane_bg_brush_);
    render_target_->CreateSolidColorBrush(theme_.splitter_color, &splitter_brush_);
    render_target_->CreateSolidColorBrush(theme_.pane_item_hover_color, &pane_item_hover_brush_);
    render_target_->CreateSolidColorBrush(theme_.pane_item_active_color, &pane_item_active_brush_);

    float thumb_alpha = is_dark ? 0.4f : 0.25f;
    D2D1_COLOR_F thumb_color = is_dark
        ? D2D1::ColorF(1.0f, 1.0f, 1.0f, thumb_alpha)
        : D2D1::ColorF(0.0f, 0.0f, 0.0f, thumb_alpha);
    render_target_->CreateSolidColorBrush(thumb_color, &scrollbar_thumb_brush_);
}

void Renderer::SetTheme(const Theme& theme) {
    theme_ = theme;
    if (!render_target_) return;
    RecreateBrushes();
    RecreatePaneFormats();
    cmd_generator_.SetTheme(&theme_);
}

void Renderer::Resize(UINT width, UINT height) {
    if (render_target_) {
        render_target_->Resize(D2D1::SizeU(width, height));
    }
}

void Renderer::SetDpi(float dpi) {
    dpi_ = dpi;
    if (render_target_) {
        render_target_->SetDpi(dpi, dpi);
    }
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

void Renderer::RecreatePaneFormats() {
    // Recreate all pane / UI text formats at updated theme sizes
    icon_font_format_.Reset();
    fmt_list_number_.Reset();
    fmt_pane_icon_.Reset();
    fmt_pane_item_.Reset();
    fmt_pane_header_.Reset();
    fmt_nav_button_.Reset();

    dwrite_factory_->CreateTextFormat(
        L"Segoe Fluent Icons", nullptr,
        DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL,
        DWRITE_FONT_STRETCH_NORMAL, theme_.font_size_body,
        L"en-us", &icon_font_format_);

    // List number format (right-aligned for ordered list bullets)
    dwrite_factory_->CreateTextFormat(
        theme_.font_family, nullptr,
        DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL,
        DWRITE_FONT_STRETCH_NORMAL, theme_.font_size_body,
        L"ja-jp", &fmt_list_number_);
    if (fmt_list_number_) {
        fmt_list_number_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_TRAILING);
    }

    dwrite_factory_->CreateTextFormat(
        L"Segoe Fluent Icons", nullptr,
        DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL,
        DWRITE_FONT_STRETCH_NORMAL, theme_.pane_font_size,
        L"en-us", &fmt_pane_icon_);
    if (fmt_pane_icon_) {
        fmt_pane_icon_->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        fmt_pane_icon_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        fmt_pane_icon_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
    }

    dwrite_factory_->CreateTextFormat(
        theme_.font_family, nullptr,
        DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL,
        DWRITE_FONT_STRETCH_NORMAL, theme_.pane_font_size,
        L"ja-jp", &fmt_pane_item_);
    if (fmt_pane_item_) {
        fmt_pane_item_->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        fmt_pane_item_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    }

    dwrite_factory_->CreateTextFormat(
        theme_.font_family, nullptr,
        DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_FONT_STYLE_NORMAL,
        DWRITE_FONT_STRETCH_NORMAL, theme_.pane_font_size,
        L"ja-jp", &fmt_pane_header_);
    if (fmt_pane_header_) {
        fmt_pane_header_->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        fmt_pane_header_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    }

    // Navigation overlay button text format (centered both axes)
    dwrite_factory_->CreateTextFormat(
        theme_.font_family, nullptr,
        DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL,
        DWRITE_FONT_STRETCH_NORMAL, theme_.pane_font_size,
        L"ja-jp", &fmt_nav_button_);
    if (fmt_nav_button_) {
        fmt_nav_button_->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        fmt_nav_button_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
        fmt_nav_button_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
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

void Renderer::ApplyVisibleEffects(std::vector<Node>& nodes, LayoutCache& cache,
                                    int first_visible, float viewport_bottom) {
    int node_count = static_cast<int>(nodes.size());
    for (int i = first_visible; i < node_count; i++) {
        if (cache[i].y_position > viewport_bottom) break;
        ApplyNodeEffects(nodes[i], cache[i]);
    }
}

ID2D1SolidColorBrush* Renderer::GetSyntaxBrush(SyntaxTokenType type) const {
    switch (type) {
        case SyntaxTokenType::Keyword:      return syntax_keyword_brush_.Get();
        case SyntaxTokenType::Type:         return syntax_type_brush_.Get();
        case SyntaxTokenType::String:       return syntax_string_brush_.Get();
        case SyntaxTokenType::Number:       return syntax_number_brush_.Get();
        case SyntaxTokenType::Comment:      return syntax_comment_brush_.Get();
        case SyntaxTokenType::Preprocessor: return syntax_preprocessor_brush_.Get();
        case SyntaxTokenType::Function:     return syntax_function_brush_.Get();
        default:                      return nullptr;
    }
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
                        cell_layout->SetDrawingEffect(link_brush_.Get(), range);
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
            entry.text_layout->SetDrawingEffect(link_brush_.Get(), range);
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
                            const PaneRect& file_pane_rect, const PaneRect& toc_pane_rect,
                            const PaneRect& md_pane_rect,
                            const std::vector<FileEntry>& file_entries,
                            const ScrollState& file_scroll, int hovered_file_index,
                            const std::vector<TocEntry>& toc_entries,
                            const ScrollState& toc_scroll, int hovered_toc_index,
                            bool show_file_pane, bool show_toc_pane) {
    if (!render_target_) return;

    render_target_->BeginDraw();
    render_target_->Clear(theme_.bg_color);

    auto size = render_target_->GetSize();

    // Draw side panes normally
    if (show_file_pane) {
        DrawFileExplorer(file_entries, file_pane_rect, file_scroll, hovered_file_index);
        DrawSplitter(file_pane_rect.x + file_pane_rect.width, size.height);
    }
    if (show_toc_pane) {
        DrawToc(toc_entries, toc_pane_rect, toc_scroll, hovered_toc_index);
        DrawSplitter(toc_pane_rect.x + toc_pane_rect.width, size.height);
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
        text_brush_->SetOpacity(alpha);
        render_target_->FillEllipse(ellipse, text_brush_.Get());
    }
    text_brush_->SetOpacity(1.0f);

    render_target_->EndDraw();
}

void Renderer::Render(std::vector<Node>& nodes, LayoutCache& cache, float scroll_y,
                      const TextSelection& selection,
                      const PaneRect& file_pane_rect, const PaneRect& toc_pane_rect,
                      const PaneRect& md_pane_rect,
                      const std::vector<FileEntry>& file_entries,
                      const ScrollState& file_scroll, int hovered_file_index,
                      const std::vector<TocEntry>& toc_entries,
                      const ScrollState& toc_scroll, int hovered_toc_index,
                      bool show_file_pane, bool show_toc_pane,
                      bool can_go_back, bool can_go_forward,
                      int nav_hovered) {
    if (!render_target_) return;

    render_target_->BeginDraw();
    render_target_->Clear(theme_.bg_color);

    auto size = render_target_->GetSize();

    // Draw file explorer pane (bitmap blit when cache is clean)
    if (show_file_pane) {
        DrawFileExplorer(file_entries, file_pane_rect, file_scroll, hovered_file_index);
        DrawSplitter(file_pane_rect.x + file_pane_rect.width, size.height);
    }

    // Draw TOC pane (bitmap blit when cache is clean)
    if (show_toc_pane) {
        DrawToc(toc_entries, toc_pane_rect, toc_scroll, hovered_toc_index);
        DrawSplitter(toc_pane_rect.x + toc_pane_rect.width, size.height);
    }

    // Binary search for first visible node (done once, shared by effects + command gen).
    float viewport_top = scroll_y;
    float viewport_bottom = scroll_y + md_pane_rect.height;
    int first_visible = 0;
    {
        int lo = 0, hi = static_cast<int>(nodes.size());
        while (lo < hi) {
            int mid = (lo + hi) / 2;
            if (cache[mid].y_position + cache[mid].height < viewport_top)
                lo = mid + 1;
            else
                hi = mid;
        }
        first_visible = lo;
    }

    // Pre-pass: apply drawing effects (syntax highlighting, link colors) on visible nodes.
    ApplyVisibleEffects(nodes, cache, first_visible, viewport_bottom);

    // Generate and execute draw commands for the Markdown content pane.
    const auto& cmds = cmd_generator_.GenerateMdPane(nodes, cache, md_pane_rect, scroll_y, selection, first_visible);
    cmd_executor_.Execute(cmds, render_target_.Get());

    // Draw navigation overlay buttons (back/forward)
    if (can_go_back || can_go_forward) {
        DrawNavOverlay(md_pane_rect, can_go_back, can_go_forward, nav_hovered);
    }

    render_target_->EndDraw();
}

// Navigation overlay constants (in DIP)
static constexpr float NAV_BTN_SIZE = 32.0f;
static constexpr float NAV_BTN_MARGIN = 16.0f;
static constexpr float NAV_BTN_GAP = 2.0f;
static constexpr float NAV_BTN_CORNER = 6.0f;

void Renderer::DrawNavOverlay(const PaneRect& md_pane_rect,
                              bool can_back, bool can_forward,
                              int hovered) {
    if (!render_target_) return;

    bool is_dark = (theme_.bg_color.r + theme_.bg_color.g + theme_.bg_color.b) < 1.5f;

    // Position: bottom-right of MD pane with margin
    float base_x = md_pane_rect.x + md_pane_rect.width - NAV_BTN_MARGIN - NAV_BTN_SIZE * 2 - NAV_BTN_GAP;
    float base_y = md_pane_rect.y + md_pane_rect.height - NAV_BTN_MARGIN - NAV_BTN_SIZE;

    // Scrollbar avoidance
    base_x -= 16.0f;

    auto drawButton = [&](float x, bool enabled, bool is_hovered, const wchar_t* arrow) {
        D2D1_RECT_F rect = D2D1::RectF(x, base_y, x + NAV_BTN_SIZE, base_y + NAV_BTN_SIZE);

        // Background
        float bg_alpha;
        if (!enabled)        bg_alpha = is_dark ? 0.08f : 0.05f;
        else if (is_hovered) bg_alpha = is_dark ? 0.35f : 0.25f;
        else                 bg_alpha = is_dark ? 0.15f : 0.10f;

        D2D1_COLOR_F bg_color = is_dark
            ? D2D1::ColorF(1.0f, 1.0f, 1.0f, bg_alpha)
            : D2D1::ColorF(0.0f, 0.0f, 0.0f, bg_alpha);

        ComPtr<ID2D1SolidColorBrush> brush;
        render_target_->CreateSolidColorBrush(bg_color, &brush);
        if (brush) {
            D2D1_ROUNDED_RECT rrect = D2D1::RoundedRect(rect, NAV_BTN_CORNER, NAV_BTN_CORNER);
            render_target_->FillRoundedRectangle(rrect, brush.Get());
        }

        // Arrow text
        float text_alpha;
        if (!enabled)        text_alpha = is_dark ? 0.2f : 0.15f;
        else if (is_hovered) text_alpha = 1.0f;
        else                 text_alpha = is_dark ? 0.6f : 0.5f;

        D2D1_COLOR_F text_color = is_dark
            ? D2D1::ColorF(1.0f, 1.0f, 1.0f, text_alpha)
            : D2D1::ColorF(0.0f, 0.0f, 0.0f, text_alpha);

        ComPtr<ID2D1SolidColorBrush> text_brush;
        render_target_->CreateSolidColorBrush(text_color, &text_brush);
        if (text_brush && fmt_nav_button_) {
            render_target_->DrawText(
                arrow, 1, fmt_nav_button_.Get(), rect, text_brush.Get(),
                D2D1_DRAW_TEXT_OPTIONS_NONE, DWRITE_MEASURING_MODE_NATURAL);
        }
    };

    // Back button (◀)
    drawButton(base_x, can_back, hovered == 1, L"\x25C0");
    // Forward button (▶)
    drawButton(base_x + NAV_BTN_SIZE + NAV_BTN_GAP, can_forward, hovered == 2, L"\x25B6");
}
