#pragma once
#include <gtest/gtest.h>
#include "layout.h"
#include "layout_computer.h"
#include "parsed_layout.h"
#include "text_measurer.h"
#include "theme.h"
#include "ui_constants.h"
#include <cmath>
#include <memory_resource>
#include <string_view>

// DirectWriteなしでLayoutEngineをテストするためのモックテキスト計測器。
// テキスト長とノード種別から高さを決定論的に計算する。
class MockTextMeasurer : public ITextMeasurer {
public:
    float line_height = 20.0f;               // 基本行高
    float chars_per_line = 40.0f;            // 折り返し推定用の1行あたり文字数
    float heading_scale = 1.5f;              // 見出し高さの倍率
    float table_row_height = 28.0f;          // テーブル1行あたりの高さ

    bool RecreateFormats() override
    {
        return true;
    }
    void UpdateTheme(const Theme& theme) noexcept override
    {
        theme_ = &theme;
    }

    void MeasureNode(const Node& node, NodeLayoutEntry& entry, float max_width,
                     MeasureViewportRange /*viewport*/ = {}) const override
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

        const auto& text = node.GetText();
        if (text.empty()) {
            // 実装と同じく空 LI は高さ 0 (bullet と直下 P の文字 Y を揃える issue#237)。
            // Init 前 (RunParallel 単体テスト等) は theme が無いので行高の半分で代用する。
            if (IsEmptyListItemContainer(node)) {
                entry.height = 0.0f;
            }
            else {
                entry.height = theme_ ? theme_->paragraph_spacing : line_height * 0.5f;
            }
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

        entry.reset_text_layout(); // モックでは実際のレイアウトなし
        entry.height = h;
        entry.layout_dirty = false;
        entry.set_effects_state(EffectsState::None);
    }

private:
    const Theme* theme_ = nullptr;

    void MeasureTable(const Node& node, NodeLayoutEntry& entry, float max_width) const
    {
        const auto* tbl = node.table_data();
        if (!tbl || tbl->row_count == 0) {
            entry.height = 0;
            entry.layout_dirty = false;
            return;
        }

        const size_t col_count = tbl->col_count;
        if (col_count == 0) {
            entry.layout_dirty = false;
            return;
        }

        auto& tl = entry.ensure_table_layout();
        const auto row_count = tbl->row_count;
        // 列幅は max_width/col_count の均等割り近似。実装 (dwrite_measurer の内容依存
        // ComputeColumnWidths) とは異なるため、cached_table_width / col_widths の絶対値は
        // 検証に使わないこと。行数・列数への依存形と border の入り方だけ実装に揃える。
        const float col_w = max_width / static_cast<float>(col_count);
        tl.col_widths.assign(col_count, col_w);
        tl.row_heights.assign(row_count, table_row_height);
        tl.col_count = col_count;
        tl.cell_layouts.resize(row_count * col_count);

        // 描画・ヒットテストは累積位置を参照するので、実装と同じ関数で積む。
        tl.RebuildRowCumY(row_count);
        tl.RebuildColCumX();
        tl.cached_table_width = tl.col_cum_x.back();

        entry.height = TABLE_BORDER_WIDTH + tl.row_cum_y[row_count];
        entry.layout_dirty = false;
    }
};

// MockTextMeasurer + LayoutEngine のフィクスチャ基底。ParseAndLayout は DWriteTestBase と同じ API。
class MockLayoutTestBase : public ::testing::Test {
protected:
    MockTextMeasurer mock_;
    LayoutEngine engine_;
    Theme theme_;

    void SetUp() override
    {
        theme_ = GetLightTheme();
        ASSERT_TRUE(engine_.Init(&mock_, theme_));
    }

    ParsedLayout ParseAndLayout(std::string_view md, float viewport_w = 800.0f)
    {
        return sources_.ParseAndLayout(engine_, md, viewport_w);
    }

private:
    LayoutSourceStore sources_;
};
