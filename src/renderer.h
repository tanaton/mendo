#pragma once
#include "types.h"
#include "theme.h"
#include "layout.h"
#include "syntax.h"
#include <d2d1.h>
#include <dwrite.h>
#include <wrl/client.h>
#include <vector>

using Microsoft::WRL::ComPtr;

class Renderer {
public:
    bool Init(HWND hwnd);
    void Resize(UINT width, UINT height);
    void Render(const std::vector<RenderNode>& nodes, float scroll_y,
                const TextSelection& selection = {});
    void SetDpi(float dpi);

    ID2D1HwndRenderTarget* GetRenderTarget() const { return render_target_.Get(); }
    LayoutEngine& GetLayout() { return layout_; }
    const Theme& GetTheme() const { return theme_; }

private:
    void DrawNode(const RenderNode& node, int node_index, float offset_x,
                  float viewport_top, float viewport_bottom, const TextSelection& selection);
    void DrawCodeBlockBackground(const RenderNode& node, float offset_x);
    void DrawHorizontalRule(const RenderNode& node, float offset_x, float content_width);
    void DrawListBullet(const RenderNode& node, float offset_x);
    void DrawBlockQuoteBar(const RenderNode& node, float base_x);
    void DrawTable(const RenderNode& node, int node_index, float offset_x, const TextSelection& selection);
    void DrawTextRangeHighlight(IDWriteTextLayout* layout, uint32_t start, uint32_t length,
                                float origin_x, float origin_y, ID2D1Brush* brush);

    ComPtr<ID2D1Factory> d2d_factory_;
    ComPtr<ID2D1HwndRenderTarget> render_target_;
    ComPtr<IDWriteFactory> dwrite_factory_;

    ComPtr<ID2D1SolidColorBrush> text_brush_;
    ComPtr<ID2D1SolidColorBrush> heading_brush_;
    ComPtr<ID2D1SolidColorBrush> code_bg_brush_;
    ComPtr<ID2D1SolidColorBrush> code_text_brush_;
    ComPtr<ID2D1SolidColorBrush> link_brush_;
    ComPtr<ID2D1SolidColorBrush> hr_brush_;
    ComPtr<ID2D1SolidColorBrush> blockquote_bar_brush_;
    ComPtr<ID2D1SolidColorBrush> blockquote_text_brush_;
    ComPtr<ID2D1SolidColorBrush> selection_brush_;
    ComPtr<ID2D1SolidColorBrush> table_stripe_brush_;

    // Syntax highlighting brushes
    ComPtr<ID2D1SolidColorBrush> syntax_keyword_brush_;
    ComPtr<ID2D1SolidColorBrush> syntax_type_brush_;
    ComPtr<ID2D1SolidColorBrush> syntax_string_brush_;
    ComPtr<ID2D1SolidColorBrush> syntax_number_brush_;
    ComPtr<ID2D1SolidColorBrush> syntax_comment_brush_;
    ComPtr<ID2D1SolidColorBrush> syntax_preprocessor_brush_;
    ComPtr<ID2D1SolidColorBrush> syntax_function_brush_;

    ID2D1SolidColorBrush* GetSyntaxBrush(SyntaxTokenType type) const;
    void ApplySyntaxHighlighting(const RenderNode& node);

    ComPtr<IDWriteTextFormat> icon_font_format_;

    Theme theme_;
    LayoutEngine layout_;
    float dpi_ = 96.0f;
    HWND hwnd_ = nullptr;
};
