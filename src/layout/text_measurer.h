#pragma once
#include "document_types.h"
#include "layout_cache.h"
#include "theme.h"

// テキスト計測の抽象インターフェース
class ITextMeasurer {
public:
    virtual ~ITextMeasurer() = default;

    virtual bool Init(const Theme& theme) = 0;
    virtual bool RecreateFormats() = 0;
    virtual void UpdateTheme(const Theme& theme) noexcept = 0;
    virtual void MeasureNode(Node& node, NodeLayoutEntry& entry, float max_width) = 0;
    virtual void MeasureTable(Node& node, NodeLayoutEntry& entry, float max_width) = 0;
};
