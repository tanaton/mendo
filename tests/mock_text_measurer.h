#pragma once
#include <gtest/gtest.h>
#include "layout_computer.h"
#include "parsed_layout.h"
#include "layout.h"
#include "theme.h"
#include "ui_constants.h"
#include <algorithm>
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

    std::optional<float> MeasureNode(const Node& node, NodeLayoutEntry& entry, float max_width,
                                     YRange /*viewport*/) const override
    {
        if (node.type == NodeType::HorizontalRule) {
            return 10.0f;
        }
        if (node.type == NodeType::Table) {
            return MeasureTable(node, entry, max_width);
        }

        const auto& text = node.GetText();
        if (text.empty()) {
            // 実装と同じ高さを使う。Init 前は theme が無いので行高の半分で代用する。
            if (theme_) {
                return layout::EmptyTextNodeHeight(node, *theme_);
            }
            return IsEmptyListItemContainer(node) ? 0.0f : line_height * 0.5f;
        }

        // テキスト長と利用可能な幅から行数を推定。コードブロックは実装と同じく折り返さない。
        float effective_chars = (max_width > 0) ? (max_width / (line_height * 0.6f)) : chars_per_line;
        effective_chars = std::max(effective_chars, 1.0f);
        const float lines = node.type == NodeType::CodeBlock
                                ? static_cast<float>(node.line_count + 1)
                                : std::ceil(static_cast<float>(text.size()) / effective_chars);
        float h = lines * line_height;

        if (node.type == NodeType::Heading) {
            h *= heading_scale;
        }

        entry.reset_text_layout(); // モックでは実際のレイアウトなし
        return h;
    }

private:
    float MeasureTable(const Node& node, NodeLayoutEntry& entry, float max_width) const
    {
        const auto* tbl = node.table_data();
        if (!tbl || tbl->row_count == 0) {
            return 0.0f;
        }

        const size_t col_count = tbl->col_count;
        if (col_count == 0) {
            return entry.height;
        }

        const auto row_count = tbl->row_count;
        auto& tl = entry.ensure_table_layout(row_count, col_count);
        // 列幅は max_width/col_count の均等割り近似。実装 (dwrite_measurer の内容依存
        // ComputeColumnWidths) とは異なるため、TableWidth() / col_widths の絶対値は
        // 検証に使わないこと。行数・列数への依存形と border の入り方だけ実装に揃える。
        const float col_w = max_width / static_cast<float>(col_count);
        tl.col_widths.assign(col_count, col_w);
        tl.row_heights.assign(row_count, table_row_height);

        // 描画・ヒットテストは累積位置を参照するので、実装と同じ関数で積む。
        tl.CommitGeometry(max_width);

        return tl.TotalHeight();
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

    // 範囲は ComputeLayout と同じ。省略すると全ノードを計測する。
    ParsedLayout ParseAndLayout(std::string_view md, float viewport_w = 800.0f,
                                float viewport_top = -YRange::kInf, float viewport_bottom = YRange::kInf)
    {
        return sources_.ParseAndLayout(engine_, md, viewport_w, viewport_top, viewport_bottom);
    }

private:
    LayoutSourceStore sources_;
};
