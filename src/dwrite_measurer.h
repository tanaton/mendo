#pragma once
#include "text_measurer.h"
#include <dwrite.h>
#include <wrl/client.h>
#include <memory_resource>

using Microsoft::WRL::ComPtr;

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
    IDWriteTextFormat* GetTextFormat(const Node& node);
    void ApplyCellRunFormatting(IDWriteTextLayout* layout, const std::pmr::vector<TextRun>& runs);

    IDWriteFactory* dwrite_ = nullptr;
    const Theme* theme_ = nullptr;

    ComPtr<IDWriteTextFormat> fmt_body_;
    ComPtr<IDWriteTextFormat> fmt_h1_;
    ComPtr<IDWriteTextFormat> fmt_h2_;
    ComPtr<IDWriteTextFormat> fmt_h3_;
    ComPtr<IDWriteTextFormat> fmt_h4_;
    ComPtr<IDWriteTextFormat> fmt_h5_;
    ComPtr<IDWriteTextFormat> fmt_h6_;
    ComPtr<IDWriteTextFormat> fmt_code_;
};
