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
    hwndProps.presentOptions = D2D1_PRESENT_OPTIONS_IMMEDIATELY;

    hr = d2d_factory_->CreateHwndRenderTarget(rtProps, hwndProps, &render_target_);
    if (FAILED(hr)) return false;

    // Create brushes
    render_target_->CreateSolidColorBrush(theme_.text_color, &text_brush_);
    render_target_->CreateSolidColorBrush(theme_.heading_color, &heading_brush_);
    render_target_->CreateSolidColorBrush(theme_.code_bg_color, &code_bg_brush_);
    render_target_->CreateSolidColorBrush(theme_.code_text_color, &code_text_brush_);
    render_target_->CreateSolidColorBrush(theme_.link_color, &link_brush_);
    render_target_->CreateSolidColorBrush(theme_.hr_color, &hr_brush_);
    render_target_->CreateSolidColorBrush(theme_.blockquote_bar_color, &blockquote_bar_brush_);
    render_target_->CreateSolidColorBrush(theme_.blockquote_text_color, &blockquote_text_brush_);
    render_target_->CreateSolidColorBrush(D2D1::ColorF(0.0f, 0.47f, 0.84f, 0.3f), &selection_brush_);
    render_target_->CreateSolidColorBrush(D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.02f), &table_stripe_brush_);

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
    render_target_->CreateSolidColorBrush(D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.25f), &scrollbar_thumb_brush_);

    // Create icon font format (Segoe Fluent Icons) for task list checkboxes
    dwrite_factory_->CreateTextFormat(
        L"Segoe Fluent Icons", nullptr,
        DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL,
        DWRITE_FONT_STRETCH_NORMAL, theme_.font_size_body,
        L"en-us", &icon_font_format_);

    // Pane icon font format (Segoe Fluent Icons at pane size)
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

    // Pane item text format
    dwrite_factory_->CreateTextFormat(
        theme_.font_family, nullptr,
        DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL,
        DWRITE_FONT_STRETCH_NORMAL, theme_.pane_font_size,
        L"ja-jp", &fmt_pane_item_);
    if (fmt_pane_item_) {
        fmt_pane_item_->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        fmt_pane_item_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    }

    // Pane header text format
    dwrite_factory_->CreateTextFormat(
        theme_.font_family, nullptr,
        DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_FONT_STYLE_NORMAL,
        DWRITE_FONT_STRETCH_NORMAL, theme_.pane_font_size,
        L"ja-jp", &fmt_pane_header_);
    if (fmt_pane_header_) {
        fmt_pane_header_->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        fmt_pane_header_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    }

    // Initialize layout engine
    if (!layout_.Init(dwrite_factory_.Get(), theme_)) return false;

    return true;
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

void Renderer::DrawCodeBlockBackground(const RenderNode& node, float offset_x, float content_width) {
    float pad = theme_.code_block_padding;
    D2D1_RECT_F bg_rect = D2D1::RectF(
        offset_x - pad,
        node.y_position - pad,
        offset_x + content_width,
        node.y_position + node.height + pad
    );
    // Rounded rectangle for code blocks
    D2D1_ROUNDED_RECT rounded = {bg_rect, 4.0f, 4.0f};
    render_target_->FillRoundedRectangle(rounded, code_bg_brush_.Get());
}

void Renderer::DrawHorizontalRule(const RenderNode& node, float offset_x, float content_width) {
    float y = node.y_position + theme_.paragraph_spacing * 0.5f;
    render_target_->DrawLine(
        D2D1::Point2F(offset_x, y),
        D2D1::Point2F(offset_x + content_width, y),
        hr_brush_.Get(),
        theme_.hr_thickness
    );
}

void Renderer::DrawListBullet(const RenderNode& node, float offset_x) {
    if (node.list_number > 0) {
        // Ordered list: draw number (reuse cached format)
        if (!fmt_list_number_) {
            dwrite_factory_->CreateTextFormat(
                theme_.font_family, nullptr,
                DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL,
                DWRITE_FONT_STRETCH_NORMAL, theme_.font_size_body,
                L"ja-jp", &fmt_list_number_);
            if (fmt_list_number_) {
                fmt_list_number_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_TRAILING);
            }
        }
        if (fmt_list_number_) {
            std::wstring num_text = std::to_wstring(node.list_number) + L".";
            D2D1_RECT_F num_rect = D2D1::RectF(
                offset_x - theme_.list_bullet_offset - 8.0f,
                node.y_position,
                offset_x - 4.0f,
                node.y_position + theme_.font_size_body * 1.5f
            );
            render_target_->DrawText(
                num_text.c_str(), static_cast<UINT32>(num_text.size()),
                fmt_list_number_.Get(), num_rect, text_brush_.Get());
        }
    } else {
        // Unordered list: draw bullet
        float bullet_y = node.y_position + theme_.font_size_body * 0.45f;
        float bullet_x = offset_x - theme_.list_bullet_offset * 0.6f;
        float r = 3.0f;
        D2D1_ELLIPSE ellipse = D2D1::Ellipse(D2D1::Point2F(bullet_x, bullet_y), r, r);

        if (node.indent_level <= 1) {
            render_target_->FillEllipse(ellipse, text_brush_.Get());
        } else {
            render_target_->DrawEllipse(ellipse, text_brush_.Get(), 1.0f);
        }
    }
}

void Renderer::DrawBlockQuoteBar(const RenderNode& node, float base_x) {
    float bar_x = base_x - theme_.indent_width * 0.5f;
    render_target_->DrawLine(
        D2D1::Point2F(bar_x, node.y_position - 2.0f),
        D2D1::Point2F(bar_x, node.y_position + node.height + 2.0f),
        blockquote_bar_brush_.Get(),
        theme_.blockquote_bar_width
    );
}

void Renderer::DrawTextRangeHighlight(IDWriteTextLayout* layout, uint32_t start, uint32_t length,
                                      float origin_x, float origin_y, ID2D1Brush* brush) {
    if (length == 0) return;
    UINT32 count = 0;
    layout->HitTestTextRange(start, length, 0, 0, nullptr, 0, &count);
    if (count == 0) return;

    std::vector<DWRITE_HIT_TEST_METRICS> metrics(count);
    layout->HitTestTextRange(start, length, 0, 0, metrics.data(), count, &count);
    for (UINT32 i = 0; i < count; i++) {
        D2D1_RECT_F rect = D2D1::RectF(
            origin_x + metrics[i].left,
            origin_y + metrics[i].top,
            origin_x + metrics[i].left + metrics[i].width,
            origin_y + metrics[i].top + metrics[i].height);
        render_target_->FillRectangle(rect, brush);
    }
}

void Renderer::DrawTable(const RenderNode& node, int node_index, float offset_x,
                         const TextSelection& selection) {
    if (node.table_rows.empty() || node.col_widths.empty()) return;

    float cell_padding = 8.0f;
    float border = 1.0f;

    // Compute total table width
    float table_width = border;
    for (float cw : node.col_widths) {
        table_width += cw + cell_padding * 2.0f + border;
    }

    // Precompute selection range for this node
    bool has_selection = selection.active &&
        node_index >= selection.start_node && node_index <= selection.end_node;
    uint32_t sel_start = 0, sel_end = static_cast<uint32_t>(node.text.size());
    if (has_selection) {
        if (node_index == selection.start_node) sel_start = selection.start_pos;
        if (node_index == selection.end_node) sel_end = selection.end_pos;
        if (sel_end <= sel_start) has_selection = false;
    }

    float y = node.y_position;
    uint32_t flat_offset = 0;

    for (size_t r = 0; r < node.table_rows.size(); r++) {
        const auto& row = node.table_rows[r];
        float row_h = row.row_height;

        // Row background: alternating + header
        bool is_header_row = (!row.cells.empty() && row.cells[0].is_header);
        if (is_header_row) {
            D2D1_RECT_F bg = D2D1::RectF(offset_x, y, offset_x + table_width, y + row_h + border);
            render_target_->FillRectangle(bg, code_bg_brush_.Get());
        } else if (r % 2 == 0) {
            D2D1_RECT_F bg = D2D1::RectF(offset_x, y, offset_x + table_width, y + row_h + border);
            render_target_->FillRectangle(bg, table_stripe_brush_.Get());
        }

        // Draw horizontal line at top of row
        render_target_->DrawLine(
            D2D1::Point2F(offset_x, y),
            D2D1::Point2F(offset_x + table_width, y),
            hr_brush_.Get(), border);

        // Draw cells
        float cx = offset_x + border;
        size_t drawn_cols = std::min(row.cells.size(), node.col_widths.size());
        for (size_t c = 0; c < drawn_cols; c++) {
            const auto& cell = row.cells[c];
            float cw = node.col_widths[c];

            // Vertical line at left of cell
            render_target_->DrawLine(
                D2D1::Point2F(cx - border, y),
                D2D1::Point2F(cx - border, y + row_h + border),
                hr_brush_.Get(), border);

            float text_x = cx + cell_padding;
            float text_y = y + cell_padding;

            // Draw selection highlight BEFORE text
            if (has_selection && cell.text_layout) {
                uint32_t cell_len = static_cast<uint32_t>(cell.text.size());
                uint32_t ov_start = std::max(sel_start, flat_offset);
                uint32_t ov_end = std::min(sel_end, flat_offset + cell_len);
                if (ov_end > ov_start) {
                    DrawTextRangeHighlight(cell.text_layout.Get(),
                        ov_start - flat_offset, ov_end - ov_start,
                        text_x, text_y, selection_brush_.Get());
                }
            }

            // Draw cell text
            if (cell.text_layout) {
                ID2D1SolidColorBrush* brush = cell.is_header ? heading_brush_.Get() : text_brush_.Get();
                render_target_->DrawTextLayout(
                    D2D1::Point2F(text_x, text_y),
                    cell.text_layout.Get(), brush);
            }

            // Advance flat_offset for this cell
            flat_offset += static_cast<uint32_t>(cell.text.size());
            if (c + 1 < row.cells.size()) flat_offset++; // tab separator

            cx += cw + cell_padding * 2.0f + border;
        }

        // Advance flat_offset for cells not drawn (beyond col_widths)
        for (size_t c = drawn_cols; c < row.cells.size(); c++) {
            flat_offset += static_cast<uint32_t>(row.cells[c].text.size());
            if (c + 1 < row.cells.size()) flat_offset++; // tab separator
        }

        // Right border
        render_target_->DrawLine(
            D2D1::Point2F(offset_x + table_width, y),
            D2D1::Point2F(offset_x + table_width, y + row_h + border),
            hr_brush_.Get(), border);

        y += row_h + border;
        if (r + 1 < node.table_rows.size()) flat_offset++; // newline separator
    }

    // Bottom border
    render_target_->DrawLine(
        D2D1::Point2F(offset_x, y),
        D2D1::Point2F(offset_x + table_width, y),
        hr_brush_.Get(), border);
}

void Renderer::DrawNode(const RenderNode& node, int node_index, float offset_x,
                        float viewport_top, float viewport_bottom,
                        const TextSelection& selection, float pane_content_width) {
    // Viewport culling
    float node_bottom = node.y_position + node.height;
    if (node_bottom < viewport_top || node.y_position > viewport_bottom) return;

    float indent = node.indent_level * theme_.indent_width;
    float x = offset_x + indent;
    float content_width = pane_content_width - indent;

    switch (node.type) {
        case NodeType::HorizontalRule:
            DrawHorizontalRule(node, x, content_width);
            return;

        case NodeType::Table:
            DrawTable(node, node_index, x, selection);
            return;

        case NodeType::CodeBlock:
            DrawCodeBlockBackground(node, x, content_width);
            ApplySyntaxHighlighting(node);
            break;

        case NodeType::ListItem:
            DrawListBullet(node, x);
            break;
        case NodeType::TaskListItem:
            break;

        case NodeType::BlockQuote:
            DrawBlockQuoteBar(node, x);
            break;

        default:
            break;
    }

    if (!node.text_layout) return;

    // Determine base brush
    ID2D1SolidColorBrush* base_brush = text_brush_.Get();
    if (node.type == NodeType::Heading) {
        base_brush = heading_brush_.Get();
    } else if (node.type == NodeType::BlockQuote) {
        base_brush = blockquote_text_brush_.Get();
    } else if (node.type == NodeType::CodeBlock) {
        base_brush = code_text_brush_.Get();
    }

    // Draw inline code backgrounds BEFORE selection and text
    for (const auto& run : node.runs) {
        if (run.code && node.type != NodeType::CodeBlock && run.length > 0) {
            UINT32 count = 0;
            node.text_layout->HitTestTextRange(run.start, run.length, 0, 0, nullptr, 0, &count);
            if (count > 0) {
                std::vector<DWRITE_HIT_TEST_METRICS> metrics(count);
                node.text_layout->HitTestTextRange(run.start, run.length, 0, 0,
                    metrics.data(), count, &count);
                for (UINT32 i = 0; i < count; i++) {
                    D2D1_RECT_F rect = D2D1::RectF(
                        x + metrics[i].left - 2.0f,
                        node.y_position + metrics[i].top - 1.0f,
                        x + metrics[i].left + metrics[i].width + 2.0f,
                        node.y_position + metrics[i].top + metrics[i].height + 1.0f
                    );
                    D2D1_ROUNDED_RECT rounded = {rect, 3.0f, 3.0f};
                    render_target_->FillRoundedRectangle(rounded, code_bg_brush_.Get());
                }
            }
        }
    }

    // Draw selection highlight AFTER code backgrounds, BEFORE text
    if (selection.active &&
        node_index >= selection.start_node && node_index <= selection.end_node) {
        uint32_t sel_start = 0;
        uint32_t sel_end = static_cast<uint32_t>(node.text.size());

        if (node_index == selection.start_node) sel_start = selection.start_pos;
        if (node_index == selection.end_node)   sel_end   = selection.end_pos;

        if (sel_end > sel_start) {
            DrawTextRangeHighlight(node.text_layout.Get(),
                sel_start, sel_end - sel_start,
                x, node.y_position, selection_brush_.Get());
        }
    }

    // Set up link underlines
    for (const auto& run : node.runs) {
        if (run.link_url.has_value()) {
            DWRITE_TEXT_RANGE range{run.start, run.length};
            node.text_layout->SetUnderline(TRUE, range);
        }
    }

    // Draw the text
    render_target_->DrawTextLayout(
        D2D1::Point2F(x, node.y_position),
        node.text_layout.Get(),
        base_brush);

    // Overdraw link text with link color
    for (const auto& run : node.runs) {
        if (run.link_url.has_value() && run.length > 0) {
            UINT32 count = 0;
            node.text_layout->HitTestTextRange(run.start, run.length, 0, 0, nullptr, 0, &count);
            if (count > 0) {
                std::vector<DWRITE_HIT_TEST_METRICS> metrics(count);
                node.text_layout->HitTestTextRange(run.start, run.length, 0, 0,
                    metrics.data(), count, &count);
                for (UINT32 i = 0; i < count; i++) {
                    D2D1_RECT_F rect = D2D1::RectF(
                        x + metrics[i].left,
                        node.y_position + metrics[i].top,
                        x + metrics[i].left + metrics[i].width,
                        node.y_position + metrics[i].top + metrics[i].height);
                    render_target_->PushAxisAlignedClip(rect, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
                    render_target_->DrawTextLayout(
                        D2D1::Point2F(x, node.y_position),
                        node.text_layout.Get(),
                        link_brush_.Get());
                    render_target_->PopAxisAlignedClip();
                }
            }
        }
    }

    // Draw task list checkbox using Segoe Fluent Icons
    if (node.type == NodeType::TaskListItem && icon_font_format_) {
        // U+E73A = CheckboxComposite (checked), U+E739 = CheckboxCompositeReversed (unchecked)
        const wchar_t icon[] = { node.task_checked ? L'\uE73A' : L'\uE739', L'\0' };
        float icon_size = theme_.font_size_body;
        float cb_x = x - theme_.list_bullet_offset;
        float cb_y = node.y_position;
        D2D1_RECT_F icon_rect = D2D1::RectF(cb_x, cb_y, cb_x + icon_size, cb_y + icon_size * 1.5f);
        render_target_->DrawText(
            icon, 1, icon_font_format_.Get(), icon_rect, text_brush_.Get());
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

void Renderer::ApplySyntaxHighlighting(const RenderNode& node) {
    if (node.syntax_tokens.empty() || !node.text_layout) return;
    for (const auto& token : node.syntax_tokens) {
        if (token.type == SyntaxTokenType::Plain) continue;
        auto* brush = GetSyntaxBrush(token.type);
        if (brush) {
            DWRITE_TEXT_RANGE range{token.start, token.length};
            node.text_layout->SetDrawingEffect(brush, range);
        }
    }
}

// Ensure a PaneCache's bitmap render target matches the required size.
// Returns true if the cache is ready to use.
static bool EnsurePaneCacheSize(Renderer::PaneCache& cache, ID2D1RenderTarget* parent,
                                float width, float height) {
    if (width <= 0 || height <= 0) return false;

    if (!cache.bitmap_rt ||
        cache.cached_width != width || cache.cached_height != height) {
        cache.bitmap_rt.Reset();
        HRESULT hr = parent->CreateCompatibleRenderTarget(
            D2D1::SizeF(width, height), &cache.bitmap_rt);
        if (FAILED(hr)) return false;
        cache.cached_width = width;
        cache.cached_height = height;
        cache.dirty = true;
    }
    return true;
}

void Renderer::DrawFileExplorer(const std::vector<FileEntry>& entries, const PaneRect& rect,
                                const ScrollState& scroll, int hovered_index) {
    if (!EnsurePaneCacheSize(file_pane_cache_, render_target_.Get(), rect.width, rect.height))
        return;

    if (file_pane_cache_.dirty) {
        auto* rt = file_pane_cache_.bitmap_rt.Get();
        rt->BeginDraw();
        rt->Clear(theme_.pane_bg_color);

        // Header background
        D2D1_RECT_F header_bg = D2D1::RectF(0, 0, rect.width, theme_.pane_header_height);
        rt->FillRectangle(header_bg, splitter_brush_.Get());
        if (fmt_pane_header_) {
            D2D1_RECT_F header_text = D2D1::RectF(8.0f, 0, rect.width - 4.0f, theme_.pane_header_height);
            rt->DrawText(L"Files", 5, fmt_pane_header_.Get(), header_text, text_brush_.Get(),
                         D2D1_DRAW_TEXT_OPTIONS_CLIP);
        }

        // Content area with clipping
        float content_top = theme_.pane_header_height;
        float content_height = rect.height - content_top;
        D2D1_RECT_F clip = D2D1::RectF(0, content_top, rect.width, rect.height);
        rt->PushAxisAlignedClip(clip, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
        rt->SetTransform(D2D1::Matrix3x2F::Translation(0, -scroll.scroll_y));

        // Viewport culling: only draw visible items
        int first = std::max(0, static_cast<int>(scroll.scroll_y / theme_.pane_item_height));
        int last = std::min(static_cast<int>(entries.size()) - 1,
            static_cast<int>((scroll.scroll_y + content_height) / theme_.pane_item_height) + 1);

        // Icon width reserved for the Segoe Fluent Icons glyph
        constexpr float icon_col_width = 24.0f;

        for (int i = first; i <= last; i++) {
            float item_y = content_top + i * theme_.pane_item_height;
            const auto& entry = entries[i];

            D2D1_RECT_F item_rect = D2D1::RectF(0, item_y, rect.width, item_y + theme_.pane_item_height);
            if (entry.is_current) {
                rt->FillRectangle(item_rect, pane_item_active_brush_.Get());
            } else if (i == hovered_index) {
                rt->FillRectangle(item_rect, pane_item_hover_brush_.Get());
            }

            // Draw icon (Segoe Fluent Icons)
            if (fmt_pane_icon_) {
                // Choose icon: folder=\uE8B7, parent(..)=\uE74A (ChevronUp), md file=\uE8A5 (Page)
                const wchar_t* icon;
                if (entry.is_parent) {
                    icon = L"\uE74A";  // ChevronUp
                } else if (entry.is_directory) {
                    icon = L"\uE8B7";  // Folder
                } else {
                    icon = L"\uE8A5";  // Page
                }
                D2D1_RECT_F icon_rect = D2D1::RectF(
                    4.0f, item_y, 4.0f + icon_col_width, item_y + theme_.pane_item_height);
                rt->DrawText(icon, 1, fmt_pane_icon_.Get(), icon_rect, text_brush_.Get(),
                             D2D1_DRAW_TEXT_OPTIONS_CLIP);
            }

            if (fmt_pane_item_) {
                D2D1_RECT_F text_rect = D2D1::RectF(
                    4.0f + icon_col_width, item_y, rect.width - 4.0f, item_y + theme_.pane_item_height);
                rt->DrawText(entry.filename.c_str(), static_cast<UINT32>(entry.filename.size()),
                             fmt_pane_item_.Get(), text_rect, text_brush_.Get(),
                             D2D1_DRAW_TEXT_OPTIONS_CLIP);
            }
        }

        rt->SetTransform(D2D1::Matrix3x2F::Identity());
        rt->PopAxisAlignedClip();

        // Draw scrollbar overlay (fixed position, not scrolled)
        float total_content = static_cast<float>(entries.size()) * theme_.pane_item_height;
        DrawPaneScrollbar(rt, rect.width, content_top, content_height,
                          scroll.scroll_y, total_content);

        rt->EndDraw();
        file_pane_cache_.dirty = false;
    }

    // Blit cached bitmap
    ComPtr<ID2D1Bitmap> bmp;
    file_pane_cache_.bitmap_rt->GetBitmap(&bmp);
    if (bmp) {
        D2D1_RECT_F dest = D2D1::RectF(rect.x, rect.y,
                                         rect.x + rect.width, rect.y + rect.height);
        render_target_->DrawBitmap(bmp.Get(), dest);
    }
}

void Renderer::DrawToc(const std::vector<TocEntry>& entries, const PaneRect& rect,
                       const ScrollState& scroll, int hovered_index) {
    if (!EnsurePaneCacheSize(toc_pane_cache_, render_target_.Get(), rect.width, rect.height))
        return;

    if (toc_pane_cache_.dirty) {
        auto* rt = toc_pane_cache_.bitmap_rt.Get();
        rt->BeginDraw();
        rt->Clear(theme_.pane_bg_color);

        // Header background
        D2D1_RECT_F header_bg = D2D1::RectF(0, 0, rect.width, theme_.pane_header_height);
        rt->FillRectangle(header_bg, splitter_brush_.Get());
        if (fmt_pane_header_) {
            D2D1_RECT_F header_text = D2D1::RectF(8.0f, 0, rect.width - 4.0f, theme_.pane_header_height);
            rt->DrawText(L"Table of Contents", 17, fmt_pane_header_.Get(), header_text,
                         text_brush_.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
        }

        // Content area with clipping
        float content_top = theme_.pane_header_height;
        float content_height = rect.height - content_top;
        D2D1_RECT_F clip = D2D1::RectF(0, content_top, rect.width, rect.height);
        rt->PushAxisAlignedClip(clip, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
        rt->SetTransform(D2D1::Matrix3x2F::Translation(0, -scroll.scroll_y));

        // Viewport culling
        int first = std::max(0, static_cast<int>(scroll.scroll_y / theme_.pane_item_height));
        int last = std::min(static_cast<int>(entries.size()) - 1,
            static_cast<int>((scroll.scroll_y + content_height) / theme_.pane_item_height) + 1);

        for (int i = first; i <= last; i++) {
            float item_y = content_top + i * theme_.pane_item_height;
            const auto& entry = entries[i];

            if (i == hovered_index) {
                D2D1_RECT_F item_rect = D2D1::RectF(0, item_y, rect.width, item_y + theme_.pane_item_height);
                rt->FillRectangle(item_rect, pane_item_hover_brush_.Get());
            }

            float indent = (entry.heading_level - 1) * 12.0f;
            if (fmt_pane_item_) {
                D2D1_RECT_F text_rect = D2D1::RectF(
                    8.0f + indent, item_y, rect.width - 4.0f, item_y + theme_.pane_item_height);
                rt->DrawText(entry.text.c_str(), static_cast<UINT32>(entry.text.size()),
                             fmt_pane_item_.Get(), text_rect, text_brush_.Get(),
                             D2D1_DRAW_TEXT_OPTIONS_CLIP);
            }
        }

        rt->SetTransform(D2D1::Matrix3x2F::Identity());
        rt->PopAxisAlignedClip();

        // Draw scrollbar overlay (fixed position, not scrolled)
        float total_content = static_cast<float>(entries.size()) * theme_.pane_item_height;
        DrawPaneScrollbar(rt, rect.width, content_top, content_height,
                          scroll.scroll_y, total_content);

        rt->EndDraw();
        toc_pane_cache_.dirty = false;
    }

    // Blit cached bitmap
    ComPtr<ID2D1Bitmap> bmp;
    toc_pane_cache_.bitmap_rt->GetBitmap(&bmp);
    if (bmp) {
        D2D1_RECT_F dest = D2D1::RectF(rect.x, rect.y,
                                         rect.x + rect.width, rect.y + rect.height);
        render_target_->DrawBitmap(bmp.Get(), dest);
    }
}

void Renderer::DrawSplitter(float x, float height) {
    D2D1_RECT_F rect = D2D1::RectF(x, 0.0f, x + theme_.splitter_width, height);
    render_target_->FillRectangle(rect, splitter_brush_.Get());
}

void Renderer::Render(const std::vector<RenderNode>& nodes, float scroll_y,
                      const TextSelection& selection,
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

    // Draw Markdown pane with clipping
    D2D1_RECT_F md_clip = D2D1::RectF(
        md_pane_rect.x, md_pane_rect.y,
        md_pane_rect.x + md_pane_rect.width, md_pane_rect.y + md_pane_rect.height);
    render_target_->PushAxisAlignedClip(md_clip, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);

    // Apply scroll transform for MD pane
    render_target_->SetTransform(D2D1::Matrix3x2F::Translation(md_pane_rect.x, -scroll_y));

    float viewport_top = scroll_y;
    float viewport_bottom = scroll_y + md_pane_rect.height;
    float offset_x = theme_.margin_left;
    float md_content_width = md_pane_rect.width - theme_.margin_left - theme_.margin_right;

    for (int i = 0; i < static_cast<int>(nodes.size()); i++) {
        DrawNode(nodes[i], i, offset_x, viewport_top, viewport_bottom, selection, md_content_width);
    }

    // Reset transform
    render_target_->SetTransform(D2D1::Matrix3x2F::Identity());
    render_target_->PopAxisAlignedClip();

    render_target_->EndDraw();
}

void Renderer::DrawPaneScrollbar(ID2D1RenderTarget* rt, float pane_width,
                                  float content_top, float content_height,
                                  float scroll_y, float total_content_height) {
    if (total_content_height <= content_height) return;

    float track_height = content_height;
    float thumb_ratio = content_height / total_content_height;
    float thumb_height = std::max(PANE_SCROLLBAR_THUMB_MIN, track_height * thumb_ratio);

    float scroll_ratio = scroll_y / (total_content_height - content_height);
    float thumb_y = content_top + scroll_ratio * (track_height - thumb_height);

    float thumb_x = pane_width - PANE_SCROLLBAR_WIDTH - 2.0f;

    D2D1_ROUNDED_RECT thumb_rect;
    thumb_rect.rect = D2D1::RectF(thumb_x, thumb_y,
                                   thumb_x + PANE_SCROLLBAR_WIDTH, thumb_y + thumb_height);
    thumb_rect.radiusX = PANE_SCROLLBAR_WIDTH / 2.0f;
    thumb_rect.radiusY = PANE_SCROLLBAR_WIDTH / 2.0f;

    rt->FillRoundedRectangle(thumb_rect, scrollbar_thumb_brush_.Get());
}
