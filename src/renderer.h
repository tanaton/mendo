#pragma once
#include "types.h"
#include "layout_cache.h"
#include "theme.h"
#include "layout.h"
#include "dwrite_measurer.h"
#include "command_generator.h"
#include "command_executor.h"
#include "syntax.h"
#include "pane.h"
#include "file_explorer.h"
#include "toc.h"
#include "mouse_gesture.h"
#include "d2d_render_backend.h"
#include <d2d1.h>
#include <dwrite.h>
#include <wrl/client.h>
#include <functional>
#include <vector>
#include <memory>

using Microsoft::WRL::ComPtr;

struct GestureRenderState {
    bool trail_active = false;
    const std::deque<GesturePoint>* trail_points = nullptr;
    bool overlay_visible = false;
    int direction = 0;   // -1=Left(戻る), 1=Right(進む)
    float overlay_alpha = 0.0f;
};

// Side-pane rendering parameters bundled into a single struct.
struct SidePaneState {
    const PaneRect& file_pane_rect;
    const PaneRect& toc_pane_rect;
    const std::vector<FileEntry>& file_entries;
    const ScrollState& file_scroll;
    int hovered_file_index;
    const std::vector<TocEntry>& toc_entries;
    const ScrollState& toc_scroll;
    int hovered_toc_index;
    bool show_file_pane;
    bool show_toc_pane;
};

class Renderer {
public:
    bool Init(HWND hwnd);
    void Resize(UINT width, UINT height);
    void Render(std::vector<Node>& nodes, LayoutCache& cache, float scroll_y,
                const TextSelection& selection,
                const PaneRect& md_pane_rect,
                const SidePaneState& side_panes,
                bool can_go_back = false, bool can_go_forward = false,
                int nav_hovered = 0,
                const GestureRenderState& gesture = {});
    void SetDpi(float dpi);
    void DrawLoading(float angle,
                     const PaneRect& md_pane_rect,
                     const SidePaneState& side_panes,
                     const GestureRenderState& gesture = {});

    ID2D1HwndRenderTarget* GetRenderTarget() const { return backend_.GetRenderTarget(); }
    LayoutEngine& GetLayout() { return layout_; }
    const Theme& GetTheme() const { return theme_; }
    void SetTheme(const Theme& theme);
    void ApplyZoom(float new_zoom);
    void ApplyZoomFromBase(const Theme& base_theme, float new_zoom);
    Theme& GetThemeMut() { return theme_; }

    // Set callback invoked when the D2D render target is recreated after device loss.
    // The callback receives the new render target pointer.
    void SetDeviceLostCallback(std::function<void(ID2D1RenderTarget*)> cb) { on_device_lost_ = std::move(cb); }

    void InvalidateFilePaneCache() { file_pane_cache_.dirty = true; }
    void InvalidateTocPaneCache() { toc_pane_cache_.dirty = true; }

private:
    // Pre-pass: apply drawing effects (syntax highlighting, link colors) to layouts.
    void ApplyVisibleEffects(std::vector<Node>& nodes, LayoutCache& cache,
                             int first_visible, float viewport_bottom);

    void DrawFileExplorer(const std::vector<FileEntry>& entries, const PaneRect& rect,
                          const ScrollState& scroll, int hovered_index);
    void DrawToc(const std::vector<TocEntry>& entries, const PaneRect& rect,
                 const ScrollState& scroll, int hovered_index);
    void DrawSplitter(float x, float height);
    void DrawPaneScrollbar(ID2D1RenderTarget* rt, float pane_width,
                           float content_top, float content_height,
                           float scroll_y, float total_content_height);
    void DrawNavOverlay(const PaneRect& md_pane_rect,
                        bool can_back, bool can_forward,
                        int hovered);  // 0=none, 1=back, 2=forward
    void DrawGestureTrail(const std::deque<GesturePoint>& points);
    void DrawGestureOverlay(int direction, float alpha, const PaneRect& md_pane_rect);

    D2DRenderBackend backend_;
    // Convenience accessors (avoid verbose backend_.Get... in 600-line rendering code)
    ID2D1HwndRenderTarget* rt() const { return backend_.GetRenderTarget(); }
    ID2D1Factory* d2d() const { return backend_.GetD2DFactory(); }

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

    // Pane brushes
    ComPtr<ID2D1SolidColorBrush> pane_bg_brush_;
    ComPtr<ID2D1SolidColorBrush> splitter_brush_;
    ComPtr<ID2D1SolidColorBrush> pane_item_hover_brush_;
    ComPtr<ID2D1SolidColorBrush> pane_item_active_brush_;
    ComPtr<ID2D1SolidColorBrush> scrollbar_thumb_brush_;

    // Reusable brush for overlay drawing (nav buttons, gesture trail/overlay).
    // Color/opacity set per use via SetColor — avoids per-frame CreateSolidColorBrush.
    ComPtr<ID2D1SolidColorBrush> overlay_brush_;

    ID2D1SolidColorBrush* GetSyntaxBrush(SyntaxTokenType type) const;
    void ApplyNodeEffects(const Node& node, NodeLayoutEntry& entry);
    void RecreateBrushes();
    void RecreatePaneFormats();
    bool CheckEndDraw();
    bool RecreateRenderTarget();

    // Hit-test buffer for ApplyNodeEffects inline-code background computation.
    std::vector<DWRITE_HIT_TEST_METRICS> hit_test_buffer_;

    ComPtr<IDWriteTextFormat> icon_font_format_;
    ComPtr<IDWriteTextFormat> fmt_list_number_;
    ComPtr<IDWriteTextFormat> fmt_pane_icon_;
    ComPtr<IDWriteTextFormat> fmt_pane_item_;
    ComPtr<IDWriteTextFormat> fmt_pane_header_;
    ComPtr<IDWriteTextFormat> fmt_nav_button_;
    ComPtr<ID2D1StrokeStyle> gesture_stroke_style_;
    ComPtr<IDWriteTextFormat> fmt_gesture_overlay_;

public:
    // Pane bitmap cache — side panes are rendered to off-screen bitmaps
    // and only re-rendered when their content changes.
    struct PaneCache {
        ComPtr<ID2D1BitmapRenderTarget> bitmap_rt;
        bool dirty = true;
        float cached_width = 0;
        float cached_height = 0;

        void Invalidate() { dirty = true; }
        void Reset() { bitmap_rt.Reset(); dirty = true; cached_width = 0; cached_height = 0; }
    };
private:
    PaneCache file_pane_cache_;
    PaneCache toc_pane_cache_;

    Theme theme_;
    DWriteTextMeasurer measurer_;
    LayoutEngine layout_;
    CommandGenerator cmd_generator_;
    CommandExecutor cmd_executor_;
    std::function<void(ID2D1RenderTarget*)> on_device_lost_;
};
