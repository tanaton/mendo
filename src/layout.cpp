#include "layout.h"
#include <algorithm>

// Named constants for magic numbers
static constexpr float MIN_COLUMN_WIDTH = 30.0f;
static constexpr float COLUMN_WIDTH_PADDING = 4.0f;
static constexpr float TABLE_CELL_PADDING = 8.0f;
static constexpr float TABLE_BORDER_WIDTH = 1.0f;
static constexpr float CODE_BLOCK_NO_WRAP_WIDTH = 10000.0f;
static constexpr float LAYOUT_MAX_HEIGHT = 100000.0f;
static constexpr float DEFAULT_COLUMN_WIDTH = 60.0f;
static constexpr float MIN_MERMAID_PLACEHOLDER_HEIGHT = 60.0f;

// ---- Free functions ----

std::vector<float> ComputeColumnWidths(const std::vector<float>& natural_widths,
                                        float available_width, size_t col_count) {
    std::vector<float> widths(col_count);
    available_width = std::max(available_width, static_cast<float>(col_count) * MIN_COLUMN_WIDTH);

    float total_natural = 0;
    for (float w : natural_widths) total_natural += w;

    if (total_natural > 0 && total_natural > available_width) {
        for (size_t c = 0; c < col_count; c++) {
            widths[c] = std::max(MIN_COLUMN_WIDTH, available_width * natural_widths[c] / total_natural);
        }
    } else {
        float even = available_width / static_cast<float>(col_count);
        for (size_t c = 0; c < col_count; c++) {
            widths[c] = std::max(natural_widths[c] + COLUMN_WIDTH_PADDING, even);
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

YPositionResult RecomputeYPositions(std::vector<Node>& nodes, LayoutCache& cache, const Theme& theme) {
    YPositionResult result;
    float y = theme.margin_top;

    for (size_t i = 0; i < nodes.size(); i++) {
        auto& entry = cache[i];
        if (entry.layout_dirty) result.has_dirty_nodes = true;

        if (nodes[i].type == NodeType::Heading) {
            y += theme.heading_spacing_above;
        }

        entry.y_position = y;
        y += entry.height;

        if (nodes[i].type == NodeType::Heading) {
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

bool LayoutEngine::RecreateFormats() {
    if (!dwrite_ || !theme_) return false;

    // Release old formats
    fmt_body_.Reset();
    fmt_h1_.Reset();
    fmt_h2_.Reset();
    fmt_h3_.Reset();
    fmt_h4_.Reset();
    fmt_h5_.Reset();
    fmt_h6_.Reset();
    fmt_code_.Reset();

    auto W = DWRITE_FONT_WEIGHT_NORMAL;
    auto B = DWRITE_FONT_WEIGHT_BOLD;

    if (FAILED(CreateFormat(dwrite_, theme_->font_family, theme_->font_size_body, W, &fmt_body_))) return false;
    if (FAILED(CreateFormat(dwrite_, theme_->font_family, theme_->font_size_h1, B, &fmt_h1_))) return false;
    if (FAILED(CreateFormat(dwrite_, theme_->font_family, theme_->font_size_h2, B, &fmt_h2_))) return false;
    if (FAILED(CreateFormat(dwrite_, theme_->font_family, theme_->font_size_h3, B, &fmt_h3_))) return false;
    if (FAILED(CreateFormat(dwrite_, theme_->font_family, theme_->font_size_h4, B, &fmt_h4_))) return false;
    if (FAILED(CreateFormat(dwrite_, theme_->font_family, theme_->font_size_h5, B, &fmt_h5_))) return false;
    if (FAILED(CreateFormat(dwrite_, theme_->font_family, theme_->font_size_h6, B, &fmt_h6_))) return false;
    if (FAILED(CreateFormat(dwrite_, theme_->monospace_font, theme_->font_size_code, W, &fmt_code_))) return false;

    fmt_body_->SetWordWrapping(DWRITE_WORD_WRAPPING_WRAP);
    fmt_h1_->SetWordWrapping(DWRITE_WORD_WRAPPING_WRAP);
    fmt_h2_->SetWordWrapping(DWRITE_WORD_WRAPPING_WRAP);
    fmt_h3_->SetWordWrapping(DWRITE_WORD_WRAPPING_WRAP);
    fmt_h4_->SetWordWrapping(DWRITE_WORD_WRAPPING_WRAP);
    fmt_h5_->SetWordWrapping(DWRITE_WORD_WRAPPING_WRAP);
    fmt_h6_->SetWordWrapping(DWRITE_WORD_WRAPPING_WRAP);
    fmt_code_->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);

    // Force width re-detection on next layout pass
    last_viewport_width_ = 0.0f;

    return true;
}

IDWriteTextFormat* LayoutEngine::GetTextFormat(const Node& node) {
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
        if (run.link_url.has_value()) layout->SetUnderline(TRUE, range);
    }
}

void LayoutEngine::CreateTableLayout(Node& node, NodeLayoutEntry& entry,
                                      DiagramEntry& /*diagram*/, float max_width) {
    if (node.table_rows.empty()) {
        entry.height = 0;
        entry.layout_dirty = false;
        return;
    }

    // Determine column count
    size_t col_count = 0;
    for (auto& row : node.table_rows) {
        col_count = std::max(col_count, row.cells.size());
    }
    if (col_count == 0) { entry.layout_dirty = false; return; }

    float cell_padding = TABLE_CELL_PADDING;
    float border_width = TABLE_BORDER_WIDTH;

    // Resize cell_layouts to match table structure
    entry.cell_layouts.resize(node.table_rows.size());
    for (size_t r = 0; r < node.table_rows.size(); r++) {
        entry.cell_layouts[r].resize(node.table_rows[r].cells.size());
    }
    entry.row_heights.resize(node.table_rows.size());

    // First pass: create text layouts and measure natural widths
    std::vector<float> natural_widths(col_count, 0.0f);
    IDWriteTextFormat* fmt = fmt_body_.Get();
    IDWriteTextFormat* fmt_bold = fmt_h4_.Get(); // reuse bold format

    for (size_t r = 0; r < node.table_rows.size(); r++) {
        auto& row = node.table_rows[r];
        for (size_t c = 0; c < row.cells.size(); c++) {
            auto& cell = row.cells[c];
            if (cell.text.empty()) continue;

            IDWriteTextFormat* cell_fmt = cell.is_header ? fmt_bold : fmt;
            dwrite_->CreateTextLayout(
                cell.text.c_str(), static_cast<UINT32>(cell.text.size()),
                cell_fmt, CODE_BLOCK_NO_WRAP_WIDTH, LAYOUT_MAX_HEIGHT,
                &entry.cell_layouts[r][c]);

            if (entry.cell_layouts[r][c]) {
                ApplyCellRunFormatting(entry.cell_layouts[r][c].Get(), cell.runs);

                DWRITE_TEXT_METRICS metrics{};
                entry.cell_layouts[r][c]->GetMetrics(&metrics);
                natural_widths[c] = std::max(natural_widths[c], metrics.width);
            }
        }
    }

    // Compute column widths: proportional distribution
    float available = max_width - (static_cast<float>(col_count) + 1.0f) * border_width
                      - static_cast<float>(col_count) * cell_padding * 2.0f;
    entry.col_widths = ComputeColumnWidths(natural_widths, available, col_count);

    // Second pass: update column widths and alignment on existing layouts, measure row heights
    float total_height = border_width; // top border
    for (size_t r = 0; r < node.table_rows.size(); r++) {
        auto& row = node.table_rows[r];
        float row_height = theme_->font_size_body * 1.4f; // minimum row height
        for (size_t c = 0; c < row.cells.size(); c++) {
            auto& cell = row.cells[c];
            float cw = (c < entry.col_widths.size()) ? entry.col_widths[c] : DEFAULT_COLUMN_WIDTH;

            if (entry.cell_layouts[r][c]) {
                entry.cell_layouts[r][c]->SetMaxWidth(cw);

                // Set text alignment
                if (cell.align == 1) entry.cell_layouts[r][c]->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
                else if (cell.align == 2) entry.cell_layouts[r][c]->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_TRAILING);

                DWRITE_TEXT_METRICS metrics{};
                entry.cell_layouts[r][c]->GetMetrics(&metrics);
                row_height = std::max(row_height, metrics.height + cell_padding * 2.0f);
            }
        }
        entry.row_heights[r] = row_height;
        total_height += row_height + border_width;
    }

    // Build linearized text for selection support (tab-separated cells, newline-separated rows)
    node.text = BuildLinearizedTableText(node.table_rows);

    entry.height = total_height;
    entry.layout_dirty = false;
}

void LayoutEngine::CreateTextLayout(Node& node, NodeLayoutEntry& entry, float max_width) {
    if (node.type == NodeType::HorizontalRule) {
        entry.height = theme_->paragraph_spacing + theme_->hr_thickness;
        entry.layout_dirty = false;
        return;
    }

    if (node.type == NodeType::Table) {
        // DiagramEntry not used for tables but we need to pass something;
        // in practice this path is only reached from ComputeLayout which passes
        // the correct DiagramEntry. For tables we can use a dummy.
        DiagramEntry dummy;
        CreateTableLayout(node, entry, dummy, max_width);
        return;
    }

    // Mermaid blocks: if a bitmap has already been rendered, use its height.
    // Otherwise set a placeholder height; the actual bitmap is created by MermaidRenderer.
    if (node.type == NodeType::CodeBlock && node.code_language == SyntaxLanguage::Mermaid) {
        // Note: diagram info is accessed via DiagramEntry in the caller.
        // Here we just handle the placeholder case when called without diagram context.
        if (entry.height > 0) {
            // Use previously known height to keep layout stable during re-render
        } else {
            entry.height = std::max(MIN_MERMAID_PLACEHOLDER_HEIGHT, theme_->font_size_body * 3.0f);
        }
        entry.layout_dirty = false;
        return;
    }

    const std::wstring& text = node.text;
    if (text.empty()) {
        entry.height = theme_->paragraph_spacing;
        entry.layout_dirty = false;
        return;
    }

    IDWriteTextFormat* fmt = GetTextFormat(node);
    float layout_width = max_width;

    // Adjust width for code blocks (account for padding)
    if (node.type == NodeType::CodeBlock) {
        layout_width = CODE_BLOCK_NO_WRAP_WIDTH; // No wrap for code
    }

    ComPtr<IDWriteTextLayout> layout;
    HRESULT hr = dwrite_->CreateTextLayout(
        text.c_str(), static_cast<UINT32>(text.size()),
        fmt, layout_width, LAYOUT_MAX_HEIGHT, &layout);

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

    entry.text_layout = std::move(layout);
    entry.height = metrics.height;
    entry.layout_dirty = false;
    entry.effects_applied = false;
    entry.inline_code_bgs.clear();
}

void LayoutEngine::ComputeLayout(std::vector<Node>& nodes, LayoutCache& cache,
                                  float viewport_width,
                                  float viewport_top, float viewport_bottom) {
    cache.Resize(nodes.size());

    bool width_changed = (viewport_width != last_viewport_width_);
    bool partial = (viewport_top >= 0.0f);

    // In partial mode, don't update last_viewport_width_ so that
    // subsequent batch processing still detects the width change
    if (!partial) {
        last_viewport_width_ = viewport_width;
    }

    float content_width = viewport_width - theme_->margin_left - theme_->margin_right;
    float y = theme_->margin_top;

    for (size_t i = 0; i < nodes.size(); i++) {
        auto& node = nodes[i];
        auto& entry = cache[i];
        float indent = node.indent_level * theme_->indent_width;
        float node_width = content_width - indent;

        bool needs_layout = width_changed || entry.layout_dirty;

        if (needs_layout) {
            if (partial) {
                // In partial mode, only compute layouts for visible nodes
                float node_bottom = y + entry.height; // estimate using old height
                bool visible = (node_bottom >= viewport_top && y <= viewport_bottom);
                if (visible) {
                    CreateTextLayout(node, entry, node_width);
                } else {
                    entry.layout_dirty = true;
                }
            } else {
                CreateTextLayout(node, entry, node_width);
            }
        }

        // Track y for partial visibility estimation
        if (node.type == NodeType::Heading) {
            y += theme_->heading_spacing_above;
        }
        y += entry.height;
        if (node.type == NodeType::Heading) {
            y += theme_->heading_spacing_below;
        } else {
            y += theme_->paragraph_spacing;
        }
    }

    auto result = RecomputeYPositions(nodes, cache, *theme_);
    total_height_ = result.total_height;
    has_dirty_nodes_ = result.has_dirty_nodes;
}

void LayoutEngine::LayoutNodes(std::vector<Node>& nodes, LayoutCache& cache, float viewport_width) {
    last_viewport_width_ = 0.0f; // Force width change detection
    ComputeLayout(nodes, cache, viewport_width + theme_->margin_left + theme_->margin_right);
}

bool LayoutEngine::EnsureVisibleLayout(std::vector<Node>& nodes, LayoutCache& cache,
                                        float viewport_width,
                                        float viewport_top, float viewport_bottom) {
    float content_width = viewport_width - theme_->margin_left - theme_->margin_right;
    bool any_updated = false;

    // Binary search for the first node whose bottom edge >= viewport_top
    int lo = 0, hi = static_cast<int>(nodes.size());
    while (lo < hi) {
        int mid = (lo + hi) / 2;
        if (cache[mid].y_position + cache[mid].height < viewport_top)
            lo = mid + 1;
        else
            hi = mid;
    }

    for (int i = lo; i < static_cast<int>(nodes.size()); i++) {
        auto& entry = cache[i];
        if (entry.y_position > viewport_bottom) break;
        if (!entry.layout_dirty) continue;
        float indent = nodes[i].indent_level * theme_->indent_width;
        CreateTextLayout(nodes[i], entry, content_width - indent);
        any_updated = true;
    }

    if (any_updated) {
        auto result = RecomputeYPositions(nodes, cache, *theme_);
        total_height_ = result.total_height;
        has_dirty_nodes_ = result.has_dirty_nodes;
    }
    return any_updated;
}

bool LayoutEngine::ProcessDirtyBatch(std::vector<Node>& nodes, LayoutCache& cache,
                                      float viewport_width, int batch_size) {
    float content_width = viewport_width - theme_->margin_left - theme_->margin_right;
    int processed = 0;

    for (size_t i = 0; i < nodes.size(); i++) {
        auto& entry = cache[i];
        if (!entry.layout_dirty) continue;

        float indent = nodes[i].indent_level * theme_->indent_width;
        CreateTextLayout(nodes[i], entry, content_width - indent);

        if (++processed >= batch_size) break;
    }

    auto result = RecomputeYPositions(nodes, cache, *theme_);
    total_height_ = result.total_height;
    has_dirty_nodes_ = result.has_dirty_nodes;

    // Update last_viewport_width_ when all dirty nodes are processed
    if (!has_dirty_nodes_) {
        last_viewport_width_ = viewport_width;
    }
    return has_dirty_nodes_;
}
