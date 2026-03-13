#pragma once
#include "types.h"
#include "theme.h"
#include <dwrite.h>
#include <wrl/client.h>

using Microsoft::WRL::ComPtr;

class LayoutEngine {
public:
    bool Init(IDWriteFactory* dwrite_factory, const Theme& theme);
    void ComputeLayout(std::vector<RenderNode>& nodes, float viewport_width);
    float GetTotalHeight() const { return total_height_; }

private:
    void CreateTextLayout(RenderNode& node, float max_width);
    void CreateTableLayout(RenderNode& node, float max_width);
    IDWriteTextFormat* GetTextFormat(const RenderNode& node);

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

    float total_height_ = 0.0f;
    float last_viewport_width_ = 0.0f;
};
