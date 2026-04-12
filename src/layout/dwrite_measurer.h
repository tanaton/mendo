#pragma once
#include "text_measurer.h"
#include <dwrite.h>
#include <wrl/client.h>
#include <memory_resource>


// ITextMeasurerのDirectWrite実装。
// IDWriteTextFormatオブジェクトを所有し、計測用のIDWriteTextLayoutを作成する。
class DWriteTextMeasurer : public ITextMeasurer {
public:
    bool Init(const Theme& theme) override;
    bool RecreateFormats() override;
    void UpdateTheme(const Theme& theme) noexcept override { theme_ = &theme; }

    void MeasureNode(Node& node, NodeLayoutEntry& entry, float max_width) override;
    void MeasureTable(Node& node, NodeLayoutEntry& entry, float max_width) override;

    // 外部のIDWriteFactoryで初期化する（Initの前に呼び出す必要がある）。
    void SetFactory(IDWriteFactory* factory) noexcept { dwrite_ = factory; }

private:
    bool CreateAllFormats();
    IDWriteTextFormat* GetTextFormat(const Node& node) noexcept;
    void ApplyCellRunFormatting(IDWriteTextLayout* layout, const std::pmr::vector<TextRun>& runs);
    void MeasureTableCells(Node& node, NodeLayoutEntry& entry, std::pmr::vector<float>& natural_widths);
    void FinalizeTableLayout(Node& node, NodeLayoutEntry& entry, float max_width, size_t col_count, std::pmr::vector<float>& natural_widths);

    IDWriteFactory* dwrite_ = nullptr;
    const Theme* theme_ = nullptr;

    Microsoft::WRL::ComPtr<IDWriteTextFormat> fmt_body_;
    Microsoft::WRL::ComPtr<IDWriteTextFormat> fmt_h_[6];
    Microsoft::WRL::ComPtr<IDWriteTextFormat> fmt_code_;
};
