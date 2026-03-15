#include "layout.h"
#include <algorithm>

// ---- Free functions ----

std::vector<float> ComputeColumnWidths(const std::vector<float>& natural_widths,
                                        float available_width, size_t col_count) {
    std::vector<float> widths(col_count);
    available_width = std::max(available_width, static_cast<float>(col_count) * 30.0f);

    float total_natural = 0;
    for (float w : natural_widths) total_natural += w;

    if (total_natural > 0 && total_natural > available_width) {
        for (size_t c = 0; c < col_count; c++) {
            widths[c] = std::max(30.0f, available_width * natural_widths[c] / total_natural);
        }
    } else {
        float even = available_width / static_cast<float>(col_count);
        for (size_t c = 0; c < col_count; c++) {
            widths[c] = std::max(natural_widths[c] + 4.0f, even);
        }
    }
    return widths;
}

std::wstring BuildLinearizedTableText(const std::vector<TableRow>& rows) {
    std::wstring text;
    for (size_t r = 0; r < rows.size(); r++) {
        const auto& row = rows[r];
        for (size_t c = 0; c < row.cells.size(); c++) {
            if (c > 0) text += L'\t';
            text += row.cells[c].text;
        }
        if (r + 1 < rows.size()) {
            text += L'\n';
        }
    }
    return text;
}

YPositionResult RecomputeYPositions(std::vector<RenderNode>& nodes, const Theme& theme) {
    YPositionResult result;
    float y = theme.margin_top;

    for (auto& node : nodes) {
        if (node.layout_dirty) result.has_dirty_nodes = true;

        if (node.type == NodeType::Heading) {
            y += theme.heading_spacing_above;
        }

        node.y_position = y;
        y += node.height;

        if (node.type == NodeType::Heading) {
            y += theme.heading_spacing_below;
        } else {
            y += theme.paragraph_spacing;
        }
    }

    result.total_height = y + theme.margin_top;
    return result;
}

// ---- LayoutEngine ----

static HRESULT CreateFormat(IDWriteFactory* factory, const wchar_t* family,
                            float size, DWRITE_FONT_WEIGHT weight,
                            IDWriteTextFormat** out) {
    return factory->CreateTextFormat(
        family, nullptr, weight,
        DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
        size, L"ja-jp", out);
}

bool LayoutEngine::Init(IDWriteFactory* dwrite_factory, const Theme& theme) {
    dwrite_ = dwrite_factory;
    theme_ = &theme;

    auto W = DWRITE_FONT_WEIGHT_NORMAL;
    auto B = DWRITE_FONT_WEIGHT_BOLD;

    if (FAILED(CreateFormat(dwrite_, theme.font_family, theme.font_size_body, W, &fmt_body_))) return false;
    if (FAILED(CreateFormat(dwrite_, theme.font_family, theme.font_size_h1, B, &fmt_h1_))) return false;
    if (FAILED(CreateFormat(dwrite_, theme.font_family, theme.font_size_h2, B, &fmt_h2_))) return false;
    if (FAILED(CreateFormat(dwrite_, theme.font_family, theme.font_size_h3, B, &fmt_h3_))) return false;
    if (FAILED(CreateFormat(dwrite_, theme.font_family, theme.font_size_h4, B, &fmt_h4_))) return false;
    if (FAILED(CreateFormat(dwrite_, theme.font_family, theme.font_size_h5, B, &fmt_h5_))) return false;
    if (FAILED(CreateFormat(dwrite_, theme.font_family, theme.font_size_h6, B, &fmt_h6_))) return false;
    if (FAILED(CreateFormat(dwrite_, theme.monospace_font, theme.font_size_code, W, &fmt_code_))) return false;

    // Enable word wrapping
    fmt_body_->SetWordWrapping(DWRITE_WORD_WRAPPING_WRAP);
    fmt_h1_->SetWordWrapping(DWRITE_WORD_WRAPPING_WRAP);
    fmt_h2_->SetWordWrapping(DWRITE_WORD_WRAPPING_WRAP);
    fmt_h3_->SetWordWrapping(DWRITE_WORD_WRAPPING_WRAP);
    fmt_h4_->SetWordWrapping(DWRITE_WORD_WRAPPING_WRAP);
    fmt_h5_->SetWordWrapping(DWRITE_WORD_WRAPPING_WRAP);
    fmt_h6_->SetWordWrapping(DWRITE_WORD_WRAPPING_WRAP);
    fmt_code_->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);

    return true;
}

IDWriteTextFormat* LayoutEngine::GetTextFormat(const RenderNode& node) {
    if (node.type == NodeType::CodeBlock) return fmt_code_.Get();
    if (node.type == NodeType::Heading) {
        switch (node.heading_level) {
            case 1: return fmt_h1_.Get();
            case 2: return fmt_h2_.Get();
            case 3: return fmt_h3_.Get();
            case 4: return fmt_h4_.Get();
            case 5: return fmt_h5_.Get();
            case 6: return fmt_h6_.Get();
        }
    }
    return fmt_body_.Get();
}

void LayoutEngine::ApplyCellRunFormatting(IDWriteTextLayout* layout,
                                          const std::vector<TextRun>& runs) {
    for (const auto& run : runs) {
        DWRITE_TEXT_RANGE range{run.start, run.length};
        if (run.bold) layout->SetFontWeight(DWRITE_FONT_WEIGHT_BOLD, range);
        if (run.italic) layout->SetFontStyle(DWRITE_FONT_STYLE_ITALIC, range);
        if (run.code) {
            layout->SetFontFamilyName(theme_->monospace_font, range);
            layout->SetFontSize(theme_->font_size_code, range);
        }
        if (run.strikethrough) layout->SetStrikethrough(TRUE, range);
    }
}

void LayoutEngine::CreateTableLayout(RenderNode& node, float max_width) {
    if (node.table_rows.empty()) {
        node.height = 0;
        node.layout_dirty = false;
        return;
    }

    // Determine column count
    size_t col_count = 0;
    for (auto& row : node.table_rows) {
        col_count = std::max(col_count, row.cells.size());
    }
    if (col_count == 0) { node.layout_dirty = false; return; }

    float cell_padding = 8.0f;
    float border_width = 1.0f;

    // First pass: create text layouts and measure natural widths
    std::vector<float> natural_widths(col_count, 0.0f);
    IDWriteTextFormat* fmt = fmt_body_.Get();
    IDWriteTextFormat* fmt_bold = fmt_h4_.Get(); // reuse bold format

    for (auto& row : node.table_rows) {
        for (size_t c = 0; c < row.cells.size(); c++) {
            auto& cell = row.cells[c];
            if (cell.text.empty()) continue;

            IDWriteTextFormat* cell_fmt = cell.is_header ? fmt_bold : fmt;
            dwrite_->CreateTextLayout(
                cell.text.c_str(), static_cast<UINT32>(cell.text.size()),
                cell_fmt, 10000.0f, 100000.0f, &cell.text_layout);

            if (cell.text_layout) {
                ApplyCellRunFormatting(cell.text_layout.Get(), cell.runs);

                DWRITE_TEXT_METRICS metrics{};
                cell.text_layout->GetMetrics(&metrics);
                natural_widths[c] = std::max(natural_widths[c], metrics.width);
            }
        }
    }

    // Compute column widths: proportional distribution
    float available = max_width - (static_cast<float>(col_count) + 1.0f) * border_width
                      - static_cast<float>(col_count) * cell_padding * 2.0f;
    node.col_widths = ComputeColumnWidths(natural_widths, available, col_count);

    // Second pass: update column widths and alignment on existing layouts, measure row heights
    float total_height = border_width; // top border
    for (auto& row : node.table_rows) {
        float row_height = theme_->font_size_body * 1.4f; // minimum row height
        for (size_t c = 0; c < row.cells.size(); c++) {
            auto& cell = row.cells[c];
            float cw = (c < node.col_widths.size()) ? node.col_widths[c] : 60.0f;

            if (cell.text_layout) {
                cell.text_layout->SetMaxWidth(cw);

                // Set text alignment
                if (cell.align == 1) cell.text_layout->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
                else if (cell.align == 2) cell.text_layout->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_TRAILING);

                DWRITE_TEXT_METRICS metrics{};
                cell.text_layout->GetMetrics(&metrics);
                row_height = std::max(row_height, metrics.height + cell_padding * 2.0f);
            }
        }
        row.row_height = row_height;
        total_height += row_height + border_width;
    }

    // Build linearized text for selection support (tab-separated cells, newline-separated rows)
    node.text = BuildLinearizedTableText(node.table_rows);

    node.height = total_height;
    node.layout_dirty = false;
}

void LayoutEngine::CreateTextLayout(RenderNode& node, float max_width) {
    if (node.type == NodeType::HorizontalRule) {
        node.height = theme_->paragraph_spacing + theme_->hr_thickness;
        node.layout_dirty = false;
        return;
    }

    if (node.type == NodeType::Table) {
        CreateTableLayout(node, max_width);
        return;
    }

    const std::wstring& text = node.text;
    if (text.empty()) {
        node.height = theme_->paragraph_spacing;
        node.layout_dirty = false;
        return;
    }

    IDWriteTextFormat* fmt = GetTextFormat(node);
    float layout_width = max_width;

    // Adjust width for code blocks (account for padding)
    if (node.type == NodeType::CodeBlock) {
        layout_width = 10000.0f; // No wrap for code
    }

    ComPtr<IDWriteTextLayout> layout;
    HRESULT hr = dwrite_->CreateTextLayout(
        text.c_str(), static_cast<UINT32>(text.size()),
        fmt, layout_width, 100000.0f, &layout);

    if (FAILED(hr)) return;

    // Apply per-run formatting
    for (const auto& run : node.runs) {
        DWRITE_TEXT_RANGE range{run.start, run.length};

        if (run.bold) {
            layout->SetFontWeight(DWRITE_FONT_WEIGHT_BOLD, range);
        }
        if (run.italic) {
            layout->SetFontStyle(DWRITE_FONT_STYLE_ITALIC, range);
        }
        if (run.code && node.type != NodeType::CodeBlock) {
            layout->SetFontFamilyName(theme_->monospace_font, range);
            layout->SetFontSize(theme_->font_size_code, range);
        }
        if (run.strikethrough) {
            layout->SetStrikethrough(TRUE, range);
        }
    }

    DWRITE_TEXT_METRICS metrics{};
    layout->GetMetrics(&metrics);

    // Tokenize code blocks for syntax highlighting
    if (node.type == NodeType::CodeBlock && node.code_language != SyntaxLanguage::None) {
        node.syntax_tokens = Tokenize(node.text, node.code_language);
    } else {
        node.syntax_tokens.clear();
    }

    node.text_layout = std::move(layout);
    node.height = metrics.height;
    node.layout_dirty = false;
    node.effects_applied = false;
    node.inline_code_bgs.clear();
}

void LayoutEngine::ComputeLayout(std::vector<RenderNode>& nodes, float viewport_width,
                                  float viewport_top, float viewport_bottom) {
    bool width_changed = (viewport_width != last_viewport_width_);
    bool partial = (viewport_top >= 0.0f);

    // In partial mode, don't update last_viewport_width_ so that
    // subsequent batch processing still detects the width change
    if (!partial) {
        last_viewport_width_ = viewport_width;
    }

    float content_width = viewport_width - theme_->margin_left - theme_->margin_right;
    float y = theme_->margin_top;

    for (auto& node : nodes) {
        float indent = node.indent_level * theme_->indent_width;
        float node_width = content_width - indent;

        bool needs_layout = width_changed || node.layout_dirty;

        if (needs_layout) {
            if (partial) {
                // In partial mode, only compute layouts for visible nodes
                float node_bottom = y + node.height; // estimate using old height
                bool visible = (node_bottom >= viewport_top && y <= viewport_bottom);
                if (visible) {
                    CreateTextLayout(node, node_width);
                } else {
                    node.layout_dirty = true;
                }
            } else {
                CreateTextLayout(node, node_width);
            }
        }

        // Track y for partial visibility estimation
        if (node.type == NodeType::Heading) {
            y += theme_->heading_spacing_above;
        }
        y += node.height;
        if (node.type == NodeType::Heading) {
            y += theme_->heading_spacing_below;
        } else {
            y += theme_->paragraph_spacing;
        }
    }

    auto result = RecomputeYPositions(nodes, *theme_);
    total_height_ = result.total_height;
    has_dirty_nodes_ = result.has_dirty_nodes;
}

void LayoutEngine::LayoutNodes(std::vector<RenderNode>& nodes, float viewport_width) {
    last_viewport_width_ = 0.0f; // Force width change detection
    ComputeLayout(nodes, viewport_width + theme_->margin_left + theme_->margin_right);
}

bool LayoutEngine::ProcessDirtyBatch(std::vector<RenderNode>& nodes,
                                      float viewport_width, int batch_size) {
    float content_width = viewport_width - theme_->margin_left - theme_->margin_right;
    int processed = 0;

    for (auto& node : nodes) {
        if (!node.layout_dirty) continue;

        float indent = node.indent_level * theme_->indent_width;
        CreateTextLayout(node, content_width - indent);

        if (++processed >= batch_size) break;
    }

    auto result = RecomputeYPositions(nodes, *theme_);
    total_height_ = result.total_height;
    has_dirty_nodes_ = result.has_dirty_nodes;

    // Update last_viewport_width_ when all dirty nodes are processed
    if (!has_dirty_nodes_) {
        last_viewport_width_ = viewport_width;
    }
    return has_dirty_nodes_;
}
