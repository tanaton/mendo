#pragma once
#include "document_types.h"
#include "layout_cache.h"
#include "theme.h"

// テキスト計測の抽象インターフェース。
// DirectWrite依存をレイアウトロジックから分離し、モックベースのテストを可能にする。
class ITextMeasurer {
public:
    virtual ~ITextMeasurer() = default;

    virtual bool Init(const Theme& theme) = 0;
    virtual bool RecreateFormats() = 0;
    virtual void UpdateTheme(const Theme& theme) noexcept = 0;

    // 非テーブルノードのテキストレイアウトを作成・計測する。
    // entry.text_layout, entry.height, entry.layout_dirty, entry.effects_applied を設定する。
    virtual void MeasureNode(Node& node, NodeLayoutEntry& entry, float max_width) = 0;

    // テーブルセルのレイアウトを作成・計測する。
    // entry.table_layout->cell_layouts/col_widths/row_heights, entry.height, entry.layout_dirty を設定する。
    virtual void MeasureTable(Node& node, NodeLayoutEntry& entry, float max_width) = 0;
};
