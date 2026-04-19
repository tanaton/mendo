#include "dwrite_measurer.h"
#include "layout.h"
#include "parser.h"
#include "syntax.h"
#include "ui_constants.h"
#include <algorithm>
#include <ranges>

using Microsoft::WRL::ComPtr;

static constexpr float CODE_BLOCK_NO_WRAP_WIDTH = 10000.0f;
static constexpr float LAYOUT_MAX_HEIGHT = 100000.0f;
static constexpr float DEFAULT_COLUMN_WIDTH = 60.0f;
static constexpr float MIN_DIAGRAM_PLACEHOLDER_HEIGHT = 60.0f;

static HRESULT CreateFormat(IDWriteFactory* factory, const wchar_t* family,
    float size, DWRITE_FONT_WEIGHT weight,
    IDWriteTextFormat** out)
{
    return factory->CreateTextFormat(
        family, nullptr, weight,
        DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
        size, L"ja-jp", out);
}

bool DWriteTextMeasurer::CreateAllFormats()
{
    if (!dwrite_ || !theme_) {
        return false;
    }

    fmt_body_.Reset();
    for (auto& fmt : fmt_h_) {
        fmt.Reset();
    }
    fmt_code_.Reset();

    const auto W = DWRITE_FONT_WEIGHT_NORMAL;
    const auto B = DWRITE_FONT_WEIGHT_BOLD;

    if (FAILED(CreateFormat(dwrite_, theme_->font_family.c_str(), theme_->font_size_body, W, &fmt_body_))) {
        return false;
    }
    for (const auto i : std::views::iota(0, 6)) {
        if (FAILED(CreateFormat(dwrite_, theme_->font_family.c_str(), theme_->font_size_h[i], B, &fmt_h_[i]))) {
            return false;
        }
    }
    if (FAILED(CreateFormat(dwrite_, theme_->monospace_font.c_str(), theme_->font_size_code, W, &fmt_code_))) {
        return false;
    }

    fmt_body_->SetWordWrapping(DWRITE_WORD_WRAPPING_WRAP);
    for (auto& fmt : fmt_h_) {
        fmt->SetWordWrapping(DWRITE_WORD_WRAPPING_WRAP);
    }
    fmt_code_->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);

    return true;
}

bool DWriteTextMeasurer::Init(const Theme& theme)
{
    theme_ = &theme;
    return CreateAllFormats();
}

bool DWriteTextMeasurer::RecreateFormats()
{
    return CreateAllFormats();
}

IDWriteTextFormat* DWriteTextMeasurer::GetTextFormat(const Node& node) noexcept
{
    if (node.type == NodeType::CodeBlock) {
        return fmt_code_.Get();
    }
    if (node.type == NodeType::Heading) {
        if (node.heading_level >= 1 && node.heading_level <= 6) {
            return fmt_h_[node.heading_level - 1].Get();
        }
    }
    return fmt_body_.Get();
}

void DWriteTextMeasurer::ApplyRunFormatting(IDWriteTextLayout* layout,
    const std::pmr::vector<TextRun>& runs, std::optional<NodeType> node_type)
{
    for (const auto& run : runs) {
        const DWRITE_TEXT_RANGE range{ run.start, run.length };
        if (run.bold()) {
            layout->SetFontWeight(DWRITE_FONT_WEIGHT_EXTRA_BOLD, range);
        }
        if (run.italic()) {
            layout->SetFontStyle(DWRITE_FONT_STYLE_ITALIC, range);
        }
        if (run.code()) {
            // CodeBlock 内ではインラインコードフォーマットを適用しない
            if (!node_type || *node_type != NodeType::CodeBlock) {
                layout->SetFontFamilyName(theme_->monospace_font.c_str(), range);
                // Heading 内ではコードフォントサイズを変更しない（見出しサイズを維持）
                if (!node_type || *node_type != NodeType::Heading) {
                    layout->SetFontSize(theme_->font_size_code, range);
                }
            }
        }
        if (run.strikethrough()) {
            layout->SetStrikethrough(TRUE, range);
        }
        // テーブルセル（node_type なし）ではリンクの下線を適用する。
        // 通常ノードではApplyNodeEffects（描画パス）で下線＋色を一括適用するため、ここではスキップ。
        if (!node_type && run.has_link()) {
            layout->SetUnderline(TRUE, range);
        }
    }
}

void DWriteTextMeasurer::MeasureNode(Node& node, NodeLayoutEntry& entry, float max_width)
{
    if (!dwrite_ || !theme_) {
        return;
    }

    if (node.type == NodeType::HorizontalRule) {
        entry.height = theme_->paragraph_spacing + theme_->hr_thickness;
        entry.layout_dirty = false;
        return;
    }

    if (node.type == NodeType::Table) {
        MeasureTable(node, entry, max_width);
        return;
    }

    // Mermaidブロック: ビットマップがレンダリングされるまでのプレースホルダー高さ
    if (node.type == NodeType::CodeBlock && node.code_language == SyntaxLanguage::Mermaid) {
        if (entry.height <= 0) {
            entry.height = std::max(MIN_DIAGRAM_PLACEHOLDER_HEIGHT, theme_->font_size_body * 3.0f);
        }
        entry.layout_dirty = false;
        return;
    }

    // 画像ノード: 元画像サイズが設定済みならコンテンツ幅に合わせてスケール、
    // 未設定ならプレースホルダー高さ
    if (node.type == NodeType::Image) {
        if (node.has_image() && node.image_data->width > 0 && node.image_data->height > 0) {
            const float w = node.image_data->width;
            float h = node.image_data->height;
            if (w > max_width) {
                h *= max_width / w;
            }
            entry.height = h;
        }
        else if (entry.height <= 0) {
            entry.height = std::max(MIN_DIAGRAM_PLACEHOLDER_HEIGHT, theme_->font_size_body * 3.0f);
        }
        entry.layout_dirty = false;
        return;
    }

    const auto& text = node.GetText();
    if (text.empty()) {
        entry.height = theme_->paragraph_spacing;
        entry.layout_dirty = false;
        return;
    }

    IDWriteTextFormat* const fmt = GetTextFormat(node);
    float layout_width = max_width;
    if (node.type == NodeType::CodeBlock) {
        layout_width = CODE_BLOCK_NO_WRAP_WIDTH;
    }

    ComPtr<IDWriteTextLayout> layout;
    const HRESULT hr = dwrite_->CreateTextLayout(
        text.c_str(), static_cast<UINT32>(text.size()),
        fmt, layout_width, LAYOUT_MAX_HEIGHT, &layout);
    if (FAILED(hr)) {
        return;
    }

    // ラン単位のフォーマットを適用
    ApplyRunFormatting(layout.Get(), node.runs, node.type);

    // Alert ノード: アイコン文字のフォントウェイトを設定
    if (node.type == NodeType::BlockQuote && node.alert_type != AlertType::None
        && node.alert_label_length > 0) {
        const UINT32 icon_len = static_cast<UINT32>(std::wcslen(GetAlertIcon(node.alert_type)));
        const DWRITE_TEXT_RANGE icon_range{ 0, icon_len };
        layout->SetFontWeight(DWRITE_FONT_WEIGHT_NORMAL, icon_range);
    }

    DWRITE_TEXT_METRICS metrics{};
    layout->GetMetrics(&metrics);

    // コードブロックのシンタックストークン化をレイアウトパスで事前実行する。
    // 描画パス（ApplyNodeEffects）での遅延トークン化を排除し、フレーム落ちを防止する。
    if (node.type == NodeType::CodeBlock && node.syntax_tokens().empty() &&
        node.code_language != SyntaxLanguage::None &&
        node.code_language != SyntaxLanguage::Mermaid) {
        node.syntax_tokens_mut() = Tokenize(text, node.code_language);
    }

    entry.text_layout = std::move(layout);
    entry.height = metrics.height;
    entry.layout_dirty = false;
    entry.effects_applied = false;
    entry.inline_code_bgs.clear();
}

void DWriteTextMeasurer::MeasureTableCells(Node& node, NodeLayoutEntry& entry,
    std::pmr::vector<float>& natural_widths)
{
    IDWriteTextFormat* const fmt = fmt_body_.Get();
    IDWriteTextFormat* const fmt_bold = fmt_h_[3].Get();
    auto& rows = node.table_rows();
    const auto row_count = rows.size();
    auto& tl = *entry.table_layout;

    for (size_t r = 0; r < row_count; r++) {
        auto& row = rows[r];
        const auto col_count = row.cells.size();
        for (size_t c = 0; c < col_count; c++) {
            auto& cell = row.cells[c];
            if (cell.text.empty()) {
                continue;
            }

            IDWriteTextFormat* const cell_fmt = cell.is_header ? fmt_bold : fmt;
            const size_t ci = tl.CellIndex(r, c);
            dwrite_->CreateTextLayout(
                cell.text.c_str(), static_cast<UINT32>(cell.text.size()),
                cell_fmt, CODE_BLOCK_NO_WRAP_WIDTH, LAYOUT_MAX_HEIGHT,
                &tl.cell_layouts[ci]);

            if (tl.cell_layouts[ci]) {
                ApplyRunFormatting(tl.cell_layouts[ci].Get(), cell.runs, std::nullopt);
                DWRITE_TEXT_METRICS metrics{};
                tl.cell_layouts[ci]->GetMetrics(&metrics);
                natural_widths[c] = std::max(natural_widths[c], metrics.width);
            }
        }
    }
}

void DWriteTextMeasurer::FinalizeTableLayout(Node& node, NodeLayoutEntry& entry, float max_width,
    size_t col_count, std::pmr::vector<float>& natural_widths)
{
    const float cell_padding = TABLE_CELL_PADDING;
    const float border_width = TABLE_BORDER_WIDTH;
    auto& tl = *entry.table_layout;

    const float available = max_width - (static_cast<float>(col_count) + 1.0f) * border_width
        - static_cast<float>(col_count) * cell_padding * 2.0f;
    tl.col_widths = ComputeColumnWidths(natural_widths, available, col_count);

    float total_height = border_width;
    auto& rows = node.table_rows();
    const auto row_count = rows.size();
    for (size_t r = 0; r < row_count; r++) {
        auto& row = rows[r];
        float row_height = theme_->font_size_body * 1.4f;
        const auto cell_count = row.cells.size();
        for (size_t c = 0; c < cell_count; c++) {
            auto& cell = row.cells[c];
            const float cw = (c < tl.col_widths.size()) ? tl.col_widths[c] : DEFAULT_COLUMN_WIDTH;

            const size_t ci = tl.CellIndex(r, c);
            if (tl.cell_layouts[ci]) {
                tl.cell_layouts[ci]->SetMaxWidth(cw);
                if (cell.align == TableAlign::Center) {
                    tl.cell_layouts[ci]->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
                }
                else if (cell.align == TableAlign::Right) {
                    tl.cell_layouts[ci]->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_TRAILING);
                }

                DWRITE_TEXT_METRICS metrics{};
                tl.cell_layouts[ci]->GetMetrics(&metrics);
                row_height = std::max(row_height, metrics.height + cell_padding * 2.0f);
            }
        }
        tl.row_heights[r] = row_height;
        total_height += row_height + border_width;
    }

    if (node.GetText().empty()) {
        node.SetText(BuildLinearizedTableText(node.table_rows()));
    }

    // ヒットテスト高速化: 行Y累積と列X累積を事前計算
    tl.row_cum_y.resize(row_count + 1);
    {
        float ry = 0.0f;
        for (size_t r = 0; r < row_count; r++) {
            tl.row_cum_y[r] = ry;
            ry += tl.row_heights[r] + border_width;
        }
        tl.row_cum_y[row_count] = ry;
    }
    tl.col_cum_x.resize(col_count + 1);
    {
        float cx = border_width;
        for (size_t c = 0; c < col_count; c++) {
            tl.col_cum_x[c] = cx;
            cx += tl.col_widths[c] + cell_padding * 2.0f + border_width;
        }
        tl.col_cum_x[col_count] = cx;
    }

    // 行ごとのフラットテキストオフセットをプリコンピュート（ヒットテスト高速化用）
    tl.row_flat_offsets.resize(row_count);
    uint32_t flat_offset = 0;
    for (size_t r = 0; r < row_count; r++) {
        tl.row_flat_offsets[r] = flat_offset;
        const auto& row = rows[r];
        const auto cell_count = row.cells.size();
        for (size_t c = 0; c < cell_count; c++) {
            flat_offset += static_cast<uint32_t>(row.cells[c].text.size());
            if (c + 1 < cell_count) {
                flat_offset++; // タブ区切り
            }
        }
        if (r + 1 < row_count) {
            flat_offset++; // 改行区切り
        }
    }

    entry.height = total_height;
    entry.layout_dirty = false;
}

void DWriteTextMeasurer::MeasureTable(Node& node, NodeLayoutEntry& entry, float max_width)
{
    if (!dwrite_ || !theme_) {
        return;
    }

    if (node.table_rows().empty()) {
        entry.height = 0;
        entry.layout_dirty = false;
        return;
    }

    auto& rows = node.table_rows();
    const auto row_count = rows.size();
    const size_t col_count = std::ranges::max(
        rows | std::views::transform([](const auto& row) { return row.cells.size(); })
    );
    if (col_count == 0) {
        entry.layout_dirty = false;
        return;
    }

    entry.effects_applied = false;
    auto& tl = entry.ensure_table_layout();
    tl.cell_inline_code_bgs.clear();
    tl.row_heights.resize(row_count);

    // セルレイアウトが既に存在する場合は第1パス（テキストレイアウト作成）をスキップし、
    // 列幅の再計算のみ行う（リサイズ時の高速パス）。
    const bool has_existing_layouts = !tl.cell_layouts.empty() && (tl.cell_layouts.size() == row_count * col_count);
    if (has_existing_layouts) {
        // キャッシュ済み自然幅を使用し、DirectWrite呼び出しを回避
        if (tl.natural_col_widths.size() == col_count) {
            FinalizeTableLayout(node, entry, max_width, col_count, tl.natural_col_widths);
        }
        else {
            // キャッシュなし: 既存レイアウトから自然幅を再取得
            std::pmr::vector<float> natural_widths(col_count, 0.0f);
            for (size_t r = 0; r < row_count; r++) {
                const auto cell_count = rows[r].cells.size();
                for (size_t c = 0; c < cell_count && c < col_count; c++) {
                    const size_t ci = tl.CellIndex(r, c);
                    if (tl.cell_layouts[ci]) {
                        tl.cell_layouts[ci]->SetMaxWidth(CODE_BLOCK_NO_WRAP_WIDTH);
                        DWRITE_TEXT_METRICS metrics{};
                        tl.cell_layouts[ci]->GetMetrics(&metrics);
                        natural_widths[c] = std::max(natural_widths[c], metrics.width);
                    }
                }
            }
            tl.natural_col_widths = natural_widths;
            FinalizeTableLayout(node, entry, max_width, col_count, natural_widths);
        }
    }
    else {
        tl.col_count = col_count;
        tl.cell_layouts.resize(row_count * col_count);

        // 第1パス: テキストレイアウトを作成し、自然な幅を計測
        std::pmr::vector<float> natural_widths(col_count, 0.0f);
        MeasureTableCells(node, entry, natural_widths);

        // 自然幅をキャッシュ（リサイズ高速パス用）
        tl.natural_col_widths = std::move(natural_widths);

        // 第2パス: 列幅を設定し、行の高さを計測
        FinalizeTableLayout(node, entry, max_width, col_count, tl.natural_col_widths);
    }
}
