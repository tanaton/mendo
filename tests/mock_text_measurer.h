#pragma once
#include "text_measurer.h"
#include <cmath>

// DirectWriteなしでLayoutEngineをテストするためのモックテキスト計測器。
// テキスト長とノード種別から高さを決定論的に計算する。
class MockTextMeasurer : public ITextMeasurer {
public:
    float line_height = 20.0f;       // 基本行高
    float chars_per_line = 40.0f;    // 折り返し推定用の1行あたり文字数
    float heading_scale = 1.5f;      // 見出し高さの倍率
    float table_row_height = 28.0f;  // テーブル1行あたりの高さ
    float table_border = 1.0f;       // テーブル罫線の幅

    bool Init(const Theme&) override { return true; }
    bool RecreateFormats() override { return true; }
    void UpdateTheme(const Theme&) noexcept override {}

    void MeasureNode(Node& node, NodeLayoutEntry& entry, float max_width) override
    {
        if (node.type == NodeType::HorizontalRule) {
            entry.height = 10.0f;
            entry.layout_dirty = false;
            return;
        }
        if (node.type == NodeType::Table) {
            MeasureTable(node, entry, max_width);
            return;
        }

        // ダイアグラム系コードブロック: 実装と同じく既存の高さを保持する
        if (node.type == NodeType::CodeBlock && IsDiagramLanguage(node.code_language)) {
            if (entry.height <= 0) {
                entry.height = 60.0f; // プレースホルダー高さ
            }
            entry.layout_dirty = false;
            return;
        }

        // 画像ノード: 実装と同じくサイズが設定済みならスケール、未設定ならプレースホルダー
        if (node.type == NodeType::Image) {
            if (const auto* img = node.image_data(); img && img->width > 0 && img->height > 0) {
                float w = img->width;
                float h = img->height;
                if (w > max_width) {
                    h *= max_width / w;
                }
                entry.height = h;
            }
            else if (entry.height <= 0) {
                entry.height = 60.0f; // プレースホルダー高さ
            }
            entry.layout_dirty = false;
            return;
        }

        const auto& text = node.GetText();
        if (text.empty()) {
            entry.height = line_height * 0.5f;
            entry.layout_dirty = false;
            return;
        }

        // テキスト長と利用可能な幅から行数を推定
        float effective_chars = (max_width > 0) ? (max_width / (line_height * 0.6f)) : chars_per_line;
        effective_chars = std::max(effective_chars, 1.0f);
        float lines = std::ceil(static_cast<float>(text.size()) / effective_chars);
        float h = lines * line_height;

        if (node.type == NodeType::Heading) {
            h *= heading_scale;
        }

        entry.text_layout = nullptr; // モックでは実際のレイアウトなし
        entry.height = h;
        entry.layout_dirty = false;
        entry.effects_applied = false;
    }

    void MeasureTable(Node& node, NodeLayoutEntry& entry, float max_width) override
    {
        if (!node.has_table() || node.table_rows().empty()) {
            entry.height = 0;
            entry.layout_dirty = false;
            return;
        }

        size_t col_count = 0;
        for (auto& row : node.table_rows()) {
            col_count = std::max(col_count, row.cells.size());
        }
        if (col_count == 0) { entry.layout_dirty = false; return; }

        auto& tl = entry.ensure_table_layout();
        const auto row_count = node.table_rows().size();
        tl.col_widths.assign(col_count, max_width / static_cast<float>(col_count));
        tl.row_heights.assign(row_count, table_row_height);
        tl.col_count = col_count;
        tl.cell_layouts.resize(row_count * col_count);

        // dwrite_measurer.cpp と同じ規約で行先頭オフセットを埋める。
        tl.row_flat_offsets.resize(row_count);
        uint32_t flat_offset = 0;
        for (size_t r = 0; r < row_count; r++) {
            tl.row_flat_offsets[r] = flat_offset;
            TableLayoutData::AdvanceFlatOffsetInRow(node.table_rows()[r], 0,
                node.table_rows()[r].cells.size(), flat_offset);
            if (r + 1 < row_count) {
                flat_offset++;
            }
        }

        float total = table_border;
        for (size_t r = 0; r < row_count; r++) {
            total += table_row_height + table_border;
        }
        entry.height = total;
        entry.layout_dirty = false;
    }
};
