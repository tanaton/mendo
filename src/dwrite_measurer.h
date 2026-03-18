#pragma once
#include "text_measurer.h"
#include <dwrite.h>
#include <wrl/client.h>

using Microsoft::WRL::ComPtr;

// DirectWrite implementation of ITextMeasurer.
// Owns IDWriteTextFormat objects and creates IDWriteTextLayout for measurement.
class DWriteTextMeasurer : public ITextMeasurer {
public:
    bool Init(const Theme& theme) override;
    bool RecreateFormats() override;
    void UpdateTheme(const Theme& theme) override { theme_ = &theme; }

    void MeasureNode(Node& node, NodeLayoutEntry& entry, float max_width) override;
    void MeasureTable(Node& node, NodeLayoutEntry& entry, float max_width) override;

    // Initialize with an external IDWriteFactory (required before Init).
    void SetFactory(IDWriteFactory* factory) { dwrite_ = factory; }

private:
    bool CreateAllFormats();
    IDWriteTextFormat* GetTextFormat(const Node& node);
    void ApplyCellRunFormatting(IDWriteTextLayout* layout, const std::vector<TextRun>& runs);

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
