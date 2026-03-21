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
#include <array>
#include <memory>
#include <memory_resource>

using Microsoft::WRL::ComPtr;

enum class BrushId : uint8_t {
    Text, Heading, CodeBg, CodeText, Link, Hr,
    BlockquoteBar, BlockquoteText, Selection, TableStripe,
    SyntaxKeyword, SyntaxType, SyntaxString, SyntaxNumber,
    SyntaxComment, SyntaxPreprocessor, SyntaxFunction,
    PaneBg, Splitter, PaneItemHover, PaneItemActive,
    ScrollbarThumb, Overlay,
    Count
};

struct GestureRenderState {
    bool trail_active = false;
    const std::pmr::deque<GesturePoint>* trail_points = nullptr;
    bool overlay_visible = false;
    int direction = 0;   // -1=Left(戻る), 1=Right(進む)
    float overlay_alpha = 0.0f;
};

// Side-pane rendering parameters bundled into a single struct.
struct SidePaneState {
    const PaneRect& file_pane_rect;
    const PaneRect& toc_pane_rect;
    const std::pmr::vector<FileEntry>& file_entries;
    const ScrollState& file_scroll;
    int hovered_file_index;
    const std::pmr::vector<TocEntry>& toc_entries;
    const ScrollState& toc_scroll;
    int hovered_toc_index;
    bool show_file_pane;
    bool show_toc_pane;
};

class Renderer {
public:
    bool Init(HWND hwnd);
    void Resize(UINT width, UINT height);
    void Render(std::pmr::vector<Node>& nodes, LayoutCache& cache, float scroll_y,
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

    ID2D1HwndRenderTarget* GetRenderTarget() const noexcept { return backend_.GetRenderTarget(); }
    LayoutEngine& GetLayout() noexcept { return layout_; }
    const Theme& GetTheme() const noexcept { return theme_; }
    void SetTheme(const Theme& theme);
    void ApplyZoom(float new_zoom);
    void ApplyZoomFromBase(const Theme& base_theme, float new_zoom);
    Theme& GetThemeMut() noexcept { return theme_; }

    // Set callback invoked when the D2D render target is recreated after device loss.
    // The callback receives the new render target pointer.
    void SetDeviceLostCallback(std::function<void(ID2D1RenderTarget*)> cb) { on_device_lost_ = std::move(cb); }

    void InvalidateFilePaneCache() noexcept { file_pane_cache_.dirty = true; }
    void InvalidateTocPaneCache() noexcept { toc_pane_cache_.dirty = true; }

private:
    // Pre-pass: apply drawing effects (syntax highlighting, link colors) to layouts.
    void ApplyVisibleEffects(std::pmr::vector<Node>& nodes, LayoutCache& cache,
                             int first_visible, float viewport_bottom);

    void DrawFileExplorer(const std::pmr::vector<FileEntry>& entries, const PaneRect& rect,
                          const ScrollState& scroll, int hovered_index);
    void DrawToc(const std::pmr::vector<TocEntry>& entries, const PaneRect& rect,
                 const ScrollState& scroll, int hovered_index);
    void DrawSplitter(float x, float height);
    void DrawNavOverlay(const PaneRect& md_pane_rect,
                        bool can_back, bool can_forward,
                        int hovered);  // 0=none, 1=back, 2=forward
    void DrawGestureTrail(const std::pmr::deque<GesturePoint>& points);
    void DrawGestureOverlay(int direction, float alpha, const PaneRect& md_pane_rect);

    D2DRenderBackend backend_;
    // Convenience accessors (avoid verbose backend_.Get... in 600-line rendering code)
    ID2D1HwndRenderTarget* rt() const noexcept { return backend_.GetRenderTarget(); }
    ID2D1Factory* d2d() const noexcept { return backend_.GetD2DFactory(); }

    std::array<ComPtr<ID2D1SolidColorBrush>, static_cast<size_t>(BrushId::Count)> brushes_;
    ID2D1SolidColorBrush* Brush(BrushId id) const noexcept {
        return brushes_[static_cast<size_t>(id)].Get();
    }

    ID2D1SolidColorBrush* GetSyntaxBrush(SyntaxTokenType type) const;
    void ApplyNodeEffects(const Node& node, NodeLayoutEntry& entry);
    void RecreateBrushes();
    void RecreatePaneFormats();
    bool CheckEndDraw();
    bool RecreateRenderTarget();

    // Hit-test buffer for ApplyNodeEffects inline-code background computation.
    std::pmr::vector<DWRITE_HIT_TEST_METRICS> hit_test_buffer_;

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

        void Invalidate() noexcept { dirty = true; }
        void Reset() noexcept { bitmap_rt.Reset(); dirty = true; cached_width = 0; cached_height = 0; }
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
